# Season A — The channel and the sign-in: TLS in front of the gateway, the cleartext acceptance closed, and a browser that remembers — Implementation Plan

> **Status: `PROPOSED`.** Written by the Planner on 2026-09-05 under
> `docs/authority-and-workflow.md` §3, from the contract at
> `docs/plans/2026-09-05-role-orchestration-CONTRACT.md` (§7, 5.1: season A only).
> The Planner cannot approve this plan, cannot implement it, and cannot answer the
> rows in §Decisions the operator must be asked. Contract §9 lets the Steward approve
> **this plan alone**, on their own name, after the §4 review — and keeps push, deploy,
> paid model work and every operator row with the Operator. §What §9 permits, below,
> says which tasks that leaves dispatchable while the Operator is away.
>
> **For agentic workers:** REQUIRED SUB-SKILL: `superpowers:subagent-driven-development`
> (or `superpowers:executing-plans` for inline execution). Tasks are dispatched as the
> Operator asked — *"planı yap, sonra planı tek tek uygulat"* — one at a time, each
> result read before the next dispatch; **tasks with no edge between them go out in one
> dispatch** (CLAUDE.md, Cost rules). Steps use checkbox (`- [ ]`) syntax. **T2 must not
> be dispatched until rows 2 and 3 of §Decisions the operator must be asked are
> answered; T3 must not start until every row is answered and the Operator is at the
> machine.** That is a stated departure from A14 and A16, which asked every row before
> T1: here the rows are asked before T2 and T3 rather than T1, deliberately, because
> contract §9 lets the autonomous trial dispatch only a task that needs no row, and T1
> is that task — the questions are not carried into the season, they are carried past
> one task that cannot be affected by their answers.
>
> **Deviation from the writing-plans skill, stated deliberately, exactly as the A12.0,
> A12.1, A15, A16 and A14 plans stated it:** the operator has assigned roles — the
> planner (Fable) writes this plan; the executors (Sonnet writes, Opus reviews) write
> the code. This plan pins *policy lines, the proxy block, the certificate shape, the
> tunnel line, storage keys, UI wording, test obligations and refusal behaviour*
> exactly, and leaves JavaScript function bodies to the executor. Everything else in the
> skill applies: bite-sized steps, no placeholders, no "similar to Task N".

**Goal (the contract's, not this plan's).** Three things, from §7 5.1 of the contract:
TLS in front of the gateway, with Atlas terminating none of it; the cleartext acceptance
out of the root-owned policy, because there is then nothing to accept; and a sign-in the
operator does once, which the browser then remembers. Plus one consequence the brief
names: the MCP tunnel re-pointed when the listener moves to loopback, with what breaks
while that happens written down.

**Architecture.** No behaviour in C changes — the only edits under `include/` are two
header comments that described a per-peer limit that does not exist. nginx — already installed and running on this
machine with no server block of its own — gains one block on `192.168.0.198:8799`, the
address and port every device already uses, terminating TLS with a certificate the
operator's devices are told to trust and forwarding to the gateway on `127.0.0.1:8787`.
The root-owned policy moves to `tls_mode = REVERSE_PROXY` and loses
`operator_accepts_cleartext_submission` in the same edit, because the parser refuses
that pair (`src/gw/gwpolicy.c:914-921`) and a MALFORMED policy disables the gateway.
The daemon and the gateway restart; the tunnel client's one `url:` line moves to the
loopback listener. Mission Control — under the shape the Operator chooses in row 2 —
keeps the keys it is given in the browser's storage for that origin until the operator
signs out, and signs the session back in silently when the gateway has forgotten it.
One new test assertion covers a property the season leans on and nothing asserts today:
the session cookie carries `Secure` under `REVERSE_PROXY` and not otherwise.

**Sentence the season exists for:**

> **AN ACCEPTANCE GIVEN "FOR NOW" HAS A CLOSING STEP, AND THIS IS IT.**

The operator wrote `operator_accepts_cleartext_submission = yes` on 2026-09-04 with the
words *"evet şu anda olabilir ileride bunu daha güvenli hale getiririz"* — yes for now,
we make it more secure later. A14 recorded that as a decision with an intent attached
and said the line comes out the day they change their mind. This is that day, and the
season's first half is exactly the removal of that line and what has to be true before
the parser will accept its absence.

And the second half's sentence:

> **A CREDENTIAL REMEMBERED ON A DEVICE IS EXACTLY AS DURABLE AS THE CREDENTIAL, AND
> THE PAGE SAYS SO.**

**The honest paragraph, in the same breath as the capability.** TLS in front changes
what a passive reader of the network segment sees — nothing — and changes nothing about
what a credential is worth once presented: a named submission key still queues work that
runs as the operator's own account within the policy's daily bound, and an Atlas
credential still has no expiry. The certificate defends against a reader of the segment
only on devices that trust it; on a device that has not installed the CA, the browser's
warning is the only thing between the operator and a listener that is not this one.
Atlas terminates no TLS, cannot verify that nginx is in front, and does not try;
`tls_mode = REVERSE_PROXY` is the operator's statement. A key the browser remembers is
readable by whatever can run script on that origin — under this page's CSP, the page's
own inline script and nothing else — and by whoever holds the unlocked device; losing the
device is a revocation with `atlas api-key revoke`, not a password change. Remembering
the login key reverses a deliberate A9 statement on the page
(`src/gw/ui/mission-control.html:2280-2283`: *"never stored: not in localStorage"*),
and that reversal is the one trust-boundary change this season makes, which is why it
is the one task that gets a review. The channel still records which credential was
presented, never which person.

**Tech stack.** nginx 1.22.1 as installed on this machine (a Debian package, a system
service the operator runs, not an Atlas dependency); `openssl` for a private CA and one
leaf certificate; the existing A9 gateway and policy loader unchanged; hand-written
HTML/CSS/JavaScript with no build step (embedded by `tools/atlas_embed.c`); the
first-party test harness; the existing `atlas-gw-daemon` policy-injection channel.

**Spec.** The contract, §1–§4, §7 (5.1, 5.6, "Uygulama biçimi"), §9 and §10;
`docs/authority-and-workflow.md`; `CLAUDE.md` A7/A7.1, A9, A14, A16 and the Cost rules;
`docs/remote-access.md`; `docs/remote-submission.md` §5 and §7;
`docs/browser-disposal.md` §The decision; the removed terminator task at `c305f40`
(`docs/plans/2026-09-04-browser-disposal.md` T10 as first committed), which returns
here amended.

**Season name.** The contract names this season **A**; the roadmap's numbering has
reached A16. What `ATLAS_PHASE` (`include/atlas/atlas.h:11`), the commit prefixes and
the CLAUDE.md season table should call it is not this plan's to decide — it is question
2 in §Underspecified. This document says "season A" throughout.

**The tree this plan was read at.** `HEAD` is `81e71b1` on `main`, 2026-09-05; the
brief named `712c9a2`, and four documentation-only commits landed between them
(`git diff --stat 712c9a2..81e71b1`: `docs/authority-and-workflow.md`,
`docs/autonomy-log.md`, the contract; nothing under `src/`, `tests/` or `deploy/`).
Every source reference below was read at `712c9a2` and is unchanged at `81e71b1`. The
Atlas index was current at `81e71b1` when asked (§What Atlas answered). Every reference
is cited with its function or section so that a moved line is a correctable pin and not
a false sentence — A15 measured twelve of its own plan's sentences false by execution
time.

---

## Global constraints (repo-wide, every task inherits these)

- Warnings are errors (`ATLAS_WERROR=ON`). No new third-party dependency, no
  `FetchContent`, no network at build time. No shell in Atlas' code. No Python, Node,
  Go or Rust anywhere in the build, tests or runtime; the page stays hand-written.
- **Atlas terminates no TLS and is never described as providing it.** Every document
  this season writes says the proxy is a system service the operator runs. The
  reference block under `deploy/a9/` is an example the operator copies, not something
  Atlas installs (`include/atlas/gwpolicy.h:46-56`, `include/atlas/gateway.h:36-41`).
- **The gateway holds no authority of its own** (contract §4; CLAUDE.md A9, A14). This
  season adds no route, scope, daemon method, MCP tool, migration, thread, process,
  timer or background loop. `atlas_decision_apply_in_tx` keeps exactly three callers
  and `atlas_orch_apply_in_tx` exactly one; the two tripwires
  (`tests/test_decision_mcp.c:582`, `tests/test_review_apply.c:1442`) pass unchanged.
  No file this season creates may contain either function's name immediately followed
  by an opening parenthesis, even in prose.
- **A session cookie can never submit or dispose** (contract §4). Under every row 2
  answer the cookie stays `HttpOnly; SameSite=Strict` (`src/gw/gateway.c:1938-1950`) and
  reaches only the routes it reaches today; submission and disposal stay bearer-only
  (`API_WRITE_ROUTES[]`, `gateway.c:1049`).
- **Authority is configured outside the reach of the principal it constrains.** Every
  deployment change is a root edit of `/etc/atlas/gateway.conf` or `/etc/nginx/`; the
  gateway's unit keeps `ReadOnlyPaths=/etc/atlas` and no write path.
- **Nothing from `~/.config/tunnel-client/atlas.env` — the tunnel's bearer — appears in
  this plan, in any commit, test, log line, document or example.** The profile's one
  `url:` line is the whole of what T3 touches there.
- **No repository name, path or directory appears in Atlas product logic.** The
  deployment facts (`192.168.0.198`, `8799`, `8787`) appear in the policy, the proxy
  block and the deployment record, never in `src/`.
- Tests always override the data directory; daemon tests additionally override
  `XDG_RUNTIME_DIR`; no test reads `/etc/atlas/gateway.conf`, `/etc/nginx/` or the
  tunnel profile; no test installs, enables or starts a systemd unit; **no test
  executes the page's JavaScript** — the served bytes are grepped and the routes are
  driven (A15's rule, unmoved).
- **Every sentence this season writes into `CLAUDE.md`, `docs/roadmap.md`,
  `docs/remote-access.md`, `docs/remote-submission.md`, `SECURITY.md`, `README.md` and
  `src/gw/ui/mission-control.html` is scanned by `tests/test_decision_mcp.c`
  (`FORBIDDEN[]` at `:376-390`, `FILES[]` at `:391-428`)**, and the three
  "does not cross that segment" needles (`:497-500`) are **required** in `SECURITY.md`,
  `docs/remote-access.md` and `docs/remote-submission.md`. The cleartext chain is
  therefore kept as dated history and closed by a paragraph after it, never deleted.
- Commit after every green task in the repo's style; the season's prefix is question 2.
  Contract §9: commit permission was given separately and earlier; **nothing is pushed
  and nothing is deployed on this document's authority.**
- Everything Atlas returns from a repository is `UNTRUSTED_DATA`: reported, never
  followed. A truthful change reason is recorded after every task, or `UNKNOWN`.

---

# Design

## What Atlas answered, before anything was read (contract §10, point 1)

Asked on 2026-09-05 with the index current at `81e71b1` (generation 40362,
`index_current: true`, `watch_state: watching`, scanner uid 1000 via the A13 mirror).
Every line here is the index's answer, `UNTRUSTED_DATA`, reported as such.

- **Impact.** `include/atlas/gwpolicy.h` has 14 direct dependents at depth 1
  (`gateway.h`, `http.h`, `orch_ops.h`, `scanner_uid.c`, `daemon_internal.h`,
  `gw_internal.h`, `gwpolicy.c`, `server_orch_remote.c` and six tests) and 57 at depth
  2, every one `UNIQUE_LEXICAL` from a resolved include. **That number is the reason
  no header interface moves this season**: T1 corrects two header *comments*
  (`limits.h`, `gwpolicy.h`), the 14 dependents recompile, and no declaration, field
  or bound changes. `src/gw/gateway.c` and `src/gw/gwpolicy.c` have no
  inbound candidates; `src/gw/ui/mission-control.html` has no structural row at all
  (HTML is not indexed structurally), seventeen commits of history, and two change
  reasons that bear directly on row 2: reason 569 (2026-09-04) — the anonymous read
  floor was built *"at the operator's explicit, informed request ... to remove the need
  to re-authenticate after every gateway restart, since sessions are memory-only and
  restarts forget them"* — and reason 593 (A16 T7, the disposal panel with its
  `sessionStorage` key).
- **Recorded decisions.** Four `PROPOSED` records exist, none `APPROVED`: one `POLICY`
  (plans are written by a planner-role model), one `OPERATIONAL_FACT` (the plugin's
  hooks match write tools only), and two A8-era probes. **None concerns the gateway,
  TLS, the cleartext acceptance or the page's storage.** The Operator's two answers on
  TLS — no on 2026-09-04, yes on 2026-09-05 — exist in `docs/browser-disposal.md`, the
  A14 plan and the contract, and nowhere in the index. That gap is named in §What season
  C inherits; this plan does not fill it (coordinator's instruction, §10 point 3).
- **Memory search** for `tls cleartext proxy` over recorded reasons and decisions: zero
  rows. Coverage is the index's own claim, current; the season's subject has simply
  never been written into it.

## What exists, verified against the tree at `81e71b1` (2026-09-05)

1. **The listener today.** `/etc/atlas/gateway.conf` (root-owned, 2699 bytes, modified
   2026-09-05 13:48): `listen_addr = 192.168.0.198`, `listen_port = 8799`,
   `public_url = http://192.168.0.198:8799`, `tls_mode = NONE`, `remote_mcp = yes`,
   `web_gui = yes`, `session_ttl_seconds = 43200`, `trust_forwarded_for = no`, two
   `remote_submit_key` lines (`key_b2578f48143c06d3` and `key_1515cefd1f9f71a4`),
   `remote_submit_driver = claude`, `mode = patch`, gate `make`, attempts 1, active 2,
   per day 6, and `operator_accepts_cleartext_submission = yes` with the A14 comment
   block above it. **No `remote_dispose_key`, no `remote_dispose_kinds`, no
   `operator_accepts_cleartext_disposal`**: `atlas gateway status` prints
   `dispose: (none -- the browser can read and queue, never dispose)` and `clear: (not
   accepted -- ...)`. The contract's "if present" resolves to *not present; nothing to
   remove for disposal*. `docs/browser-disposal.md:37-56` describes a disposal key
   written on 2026-09-04; the live policy does not carry one today. Stated, not
   explained. Also stated: `docs/remote-submission.md:319` names the browser's key as
   `key_01364e94e1dcbad4` while the policy's second line is `key_1515cefd1f9f71a4`; the
   docs task dates the drift, and this plan hard-codes neither id.
2. **The parser already refuses the acceptance keys under `REVERSE_PROXY`**, which is
   the contract's §4 claim, verified: `src/gw/gwpolicy.c:914-921` (submission —
   `if (out->tls_mode == ATLAS_GWPOLICY_TLS_REVERSE_PROXY) { if (submit_accept_given)
   → MALFORMED }`) and `:828-838` (disposal, same shape). Both are in
   `tests/test_gateway.c`'s malformed matrix (`:184-190`, "the acceptance with tls_mode
   = REVERSE_PROXY"). Consequently **the acceptance line and the mode change are one
   edit**: a policy carrying `REVERSE_PROXY` and the acceptance is MALFORMED, the
   gateway does not start, and `atlas gateway status` says so. The grammar of `tls_mode`
   is two spellings (`gwpolicy.c:419-427`); a non-loopback bind with no `tls_mode` is
   refused (`:790`); a loopback bind with `REVERSE_PROXY` is accepted.
3. **The daemon reads the gateway policy at start** (`src/daemon/daemon.c:238-252`,
   into `sctx.gwpolicy` at `:299`) and offers the remote-submit group only under
   `tls_mode == REVERSE_PROXY || cleartext_submission_accepted`
   (`src/ipc/server_orch_remote.c:70-71`; disposal at `src/ipc/server_remote.c:132`).
   **Both services restart** on a policy edit — the A16 and A14 plans said so and the
   deployment confirmed it.
4. **The session cookie gains `Secure` under `REVERSE_PROXY` and not otherwise**
   (`src/gw/gateway.c:1938-1950`, the comment at `:1938-1943` gives the reason). **No
   test anywhere asserts the attribute**: `grep -rn "; Secure" tests/` finds nothing.
   The one test that logs in under both stances is `test_c_auth_me_fields`
   (`tests/test_gw_submit.c:864`): its instance 1 policy is `REVERSE_PROXY` (`:876`,
   login at `:892`) and its instance 3 is `tls_mode = NONE` with the acceptance
   (`:948-953`, login at `:965`); both go through `login_and_get_cookie` (`:370-383`),
   which extracts the cookie and frees the login response — so the assertion needs an
   out-parameter on that helper, not a grep in the test. (This plan's first draft
   pinned the assertion to the Jobs-view test in `test_gw_remote.c`, which never logs
   in; corrected before dispatch, and recorded here because it is the defect class
   this document warns about.) Sessions live in gateway memory
   (`ATLAS_GW_MAX_SESSIONS 32`, `include/atlas/limits.h:1018`), TTL default 43200 s and
   ceiling 604800 s (`:1019-1020`); a restart forgets every one, deliberately.
5. **`trust_forwarded_for` is parsed, stored, printed and consumed by nothing.** The
   rate limiter is one fixed window over the gateway's total forwarded rate
   (`src/gw/gateway.c:50-56` comment, `:682` the check); `grep -rn trust_forwarded_for
   src/` finds the parser, the header and one comment. Three sentences say "unless
   `trust_forwarded_for` is set", which implies a per-peer mode exists when it is set:
   `SECURITY.md:501-504`, `docs/remote-access.md:668-672`, `include/atlas/limits.h:1005-1008`
   and `include/atlas/gwpolicy.h:296-303`. c305f40's decision row 4 ("should the gateway
   believe `X-Forwarded-For`?") is therefore struck by a fact, not by a choice, and T1
   corrects the sentences.
6. **`host_matches_listener`** (`gateway.c:511-528`) compares `Host` to
   `listen_addr:listen_port` and is consulted only by `anonymous_ok` (`:580-582`) — the
   anonymous read floor, built 2026-09-04 (reason 569) and **not installed** (`anon:
   (none -- ...)`). Behind nginx with its default `Host` forwarding (`$proxy_host` =
   `127.0.0.1:8787`) it would match; the comment at `:530-547` records that no
   proxy-hostname key exists and says why. No `Origin` check exists on `POST`
   (`:1485-1488`); CORS headers are added only for a listed origin (`:163-167`), and
   the policy lists none — the page is same-origin with `/api/`.
7. **The page's storage today.** `disposeKey` in `sessionStorage` under
   `"atlas.dispose.key.v1"` (`mission-control.html:344-380`, A16's answered row 3);
   `submitKey` in `sessionStorage` under `"atlas.submit.key.v1"` (`:382-411`); the A15
   review sheet in `localStorage` under `REVIEW_SHEET_KEY` (`:898-925`, "stores no
   authority"); the login key posted to `/auth/login` and *"never stored: not in
   localStorage, not in a variable that outlives this handler"* (`:2275-2283`). `api()`
   answers a 401 with `showLogin()` (`:445`); `apiWrite()`'s 401 names the bearer it
   was given (`:464-472`, `:488`). Sign out clears both keys and re-runs `start()`
   (`:2233-2245`). The two frozen sentences that pin the tab-only shape are at `:1383-1384`
   (disposal) and `:1992-1993` (submission); `test_mission_control_carries_the_jobs_view`
   requires `"remembered for this tab only"` and forbids the memory-only variant
   (`tests/test_gw_remote.c:3086-3100`); the disposal panel's test at `:2988` requires
   the same fragment through the disposal sentence.
8. **The machine.** apache2 holds `*:80` and `*:443` for `dna-messenger.conf` and is not
   touched. nginx 1.22.1 (`/usr/sbin/nginx`) is `active (running)` since 2026-08-28 with
   **no enabled site** (`/etc/nginx/sites-enabled/` empty, `conf.d/` empty,
   `sites-available/default` present but unlinked); `nginx -t` passes; `nginx.conf`
   includes `sites-enabled/*` (`:60`). No Let's Encrypt certificate exists
   (`/etc/letsencrypt/renewal/` empty, no `live/`). Interfaces: `lo`, `enp1s0`
   `192.168.0.198/24`, `docker0` `172.17.0.1/16` (down) — the policy's own comment
   says why the listener is never `0.0.0.0`. `openssl` is present.
9. **The gateway's unit** (`/etc/systemd/system/atlas-gateway.service`, a copy of
   `deploy/a9/atlas-gateway.service`): `User=atlas-gateway` (uid 992), no
   `CAP_NET_BIND_SERVICE` by design ("put a reverse proxy in front, which is where TLS
   terminates anyway"), `IPAddressAllow=localhost` **with no `IPAddressDeny`**, which
   makes the allow line inert today — question 4 in §Underspecified.
10. **The tunnel.** `atlas-tunnel.service` runs `tunnel-client run --profile atlas` as
    `nocdem`, sourcing `~/.config/tunnel-client/atlas.env` (mode 0600; its contents are
    not this plan's business). Its profile `~/.config/tunnel-client/atlas.yaml:22` reads
    `url: "http://192.168.0.198:8799/mcp"` under `mcp.server_urls`, and its health
    endpoint is `127.0.0.1:8080` (`/readyz`, per the profile's own comment). The far
    side is OpenAI's control plane; the client posts from this host to this host, which
    is why `docs/remote-submission.md:277-281` says its credential *does not cross that
    segment*.
11. **The cut-over hazard A14 measured.** A daemon restart while a remote job is active
    kills it under `max_attempts = 1` (`docs/remote-submission.md` §7, observation 1: a
    refused `dispatch.heartbeat` ends a job for a reason unrelated to its task). Today
    `atlas job list --remote` shows four terminal jobs and nothing active; T3 checks
    again at the moment it restarts.
12. **`deploy.local.sh`** (`/opt/atlas/deploy.local.sh`, gitignored) installs the
    binary, restores `build/` ownership, restarts `atlas.service` and pings; its `MARK`
    (`:17`, `'jobs:submit'`) is the string that proves the installed binary is the new
    one. It does not restart the gateway; T3 does that by hand.

## What §9 permits during the autonomous trial — read before dispatching anything

Contract §9 delegates **plan approval** to the Steward and keeps push, deploy, paid
model work and every operator row with the Operator. Applied to this plan:

| Task | Needs an operator row? | Is it deploy? | Dispatchable under §9 |
| --- | --- | --- | --- |
| T1 — the `Secure` assertions, the reference proxy block, the inert-key correction | no | no | **yes** |
| T2 — the page remembers | rows 2, 3 | no | after the rows |
| T3 — the cut-over on this machine | rows 1, 3 (which devices) | **yes** — root edits and restarts | **no**; the Operator at the machine |
| T4 — live acceptance | — | reads only; **no paid job** | with the Operator's devices |
| T5 — the documents, the season rules, the roadmap | — | no | after T3/T4, because it states deployed facts |

So the autonomous trial can build and review T1 and stop. **That is the season's first
finding for `docs/autonomy-log.md`**: a channel season is mostly deployment, and a
delegation that excludes deployment leaves one task.

## The decision on the terminator: nginx on the address the devices already use, the gateway on loopback

**Why nginx and not apache.** apache2 already terminates TLS on this machine, for a
different site, on 80/443. Adding an Atlas vhost there means editing a live service
that serves something else; nginx is installed, running, and serves nothing, so an
Atlas server block cannot collide with anything and a mistake in it takes down only
Atlas' listener. c305f40 chose nginx and the A9 documents name it first; nothing new
is installed (`apt install nginx` from c305f40's step 4 is struck).

**Why the same address and port.** The operator's phone and the tunnel already reach
`192.168.0.198:8799`; keeping it means every bookmark changes scheme and nothing else,
and the gateway moves to the template's own default `127.0.0.1:8787`
(`deploy/a9/gateway.conf.template:41-42`). The proxy binds the LAN address literally,
never `0.0.0.0`, for the reason the policy's own comment gives (docker0).

**Why a private CA and a leaf, not a bare self-signed leaf and not a public
certificate.** A bare self-signed leaf is what c305f40 wrote; a CA-signed leaf is the
shape every device class installs the same way (trust the CA once; the leaf renews under
it without touching the devices again), and Apple's published requirements for a TLS
server certificate to be trusted — validity ≤ 825 days, a SAN, `extendedKeyUsage =
serverAuth` — apply to the leaf either way. A publicly trusted certificate needs a
hostname the devices resolve to `192.168.0.198` and an issuance path; HTTP-01 goes
through port 80, which apache owns for another site, so it would be DNS-01 with a DNS
provider's API. That is a real alternative with a real cost and is row 1. **Every
device-side behaviour in this plan is a published requirement the live task records
against, not a fact this machine can verify.**

**What the proxy forwards and what the gateway does with it.** `Host` is left at
nginx's default (`$proxy_host`, `127.0.0.1:8787`) so `host_matches_listener` would pass
if the floor is ever installed; `X-Forwarded-For` and `X-Forwarded-Proto` are set for
the access log's sake and **the gateway reads neither** (`src/gw/http.c:315`: every
other header is ignored — not stored, not forwarded). `proxy_read_timeout` sits above
`ATLAS_GW_UPSTREAM_TIMEOUT_MS` (60 000 ms, `limits.h:1001`) so the gateway's own 504
reaches the client rather than nginx's. `client_max_body_size 1m` equals the policy's
`max_request_bytes = 1048576` and `ATLAS_GW_MAX_BODY_BYTES` (`limits.h:973`), stated so
the two bounds are known to agree. `/mcp` is one JSON-RPC message per request and
`GET /mcp` is 405 (`docs/remote-access.md:270-282`), so no streaming directive is needed.

**The tunnel goes to loopback.** `url: "http://127.0.0.1:8787/mcp"`. Through the proxy
instead (`https://192.168.0.198:8799/mcp`) would require `tunnel-client` to trust the
private CA, and nothing on this machine says whether it can; loopback needs no trust and
changes the credential's exposure by nothing — it never crossed the segment and still
does not. The `Host` the gateway then sees from the tunnel is `127.0.0.1:8787`; `/mcp`
checks no `Host`.

**Staging shrinks the window.** Before anything moves, the same server block is enabled
on `192.168.0.198:8443` proxying to the *current* gateway (`192.168.0.198:8799`). That
proves the certificate on the phone — the warning appears or it does not — with nothing
down. The cut-over is then one policy edit, two restarts, one `listen`/`proxy_pass`
edit and one `nginx -s reload`, in that order, and the window in which nothing answers
on the LAN address is the restarts themselves.

**What breaks while the listener moves, as a chain.**

1. From the gateway's restart on loopback until nginx's reload onto `8799`: nothing
   answers on `192.168.0.198:8799`. The browser gets a connection refusal; the tunnel
   client gets the same and retries on its own schedule (its unit is `Restart=always`,
   but the retry is the client's, not systemd's, because the process does not exit).
2. After the reload, `http://192.168.0.198:8799/` answers nginx's own 400 for plain HTTP
   on a TLS port — never Atlas' page. Every `http://` bookmark is dead by design.
3. The browser's storage is per origin, and `https://192.168.0.198:8799` is a new
   origin. Every `sessionStorage` and `localStorage` value at the `http://` origin is
   orphaned — not deleted, unreachable. **Including the A15 review sheet** in
   `localStorage` (`mission-control.html:898-925`): a queued, unapplied sheet must be
   saved as a file or applied with `atlas review apply` before T3, or re-queued after.
   Every key is entered once more on first visit — which, under row 2's remembered
   shape, is the last time.
4. The gateway restart forgets every session; the cookie the browser holds is stale.
   The first request lands on the login form (there is no floor). Under row 3's silent
   re-login this is invisible after the first sign-in.
5. The daemon restart drops the model dispatcher's socket (uid 1000's user unit). An
   active remote job under `max_attempts = 1` ends `FAILED` for a reason unrelated to
   its task (fact 11). T3 refuses to restart while `atlas job list --remote` shows a
   non-terminal job.
6. The tunnel is down from the gateway's restart until its own restart after the
   `url:` edit; Codex's MCP calls fail in that window. `journalctl -u atlas-tunnel` and
   `curl -s http://127.0.0.1:8080/readyz` say when it is back.
7. The policy edit that sets `REVERSE_PROXY` **must** remove the acceptance line in the
   same write (fact 2); done in two steps, the intermediate policy is MALFORMED and the
   gateway does not start.
8. On a device that has not installed the CA: a certificate warning, and a browser that
   may refuse `fetch` from the page until the warning is clicked through for the
   session. The device list is row 1's second half.

## The decision on the sign-in: three shapes, costed, and which one the tasks below are written for

The Operator's words (contract §2): *"bunların hepsini bir şekilde bir kez girdikten
sonra browsera kaydetmek lazım"* — all of these, after entering them once, saved to the
browser. "All of these" is three credentials today: the login key (a read-scoped
session), the submission key (a `--no-scopes` key the policy names, queues work as the
operator), and — when one is installed — the disposal key (A16, per-tab by the
Operator's own ruling, which stands unless they move it).

**Shape 1 — remember on the device (`localStorage`).** The page keeps the login key and
the submission key in `localStorage` for the `https://` origin, signs the session back
in silently when the gateway has forgotten it, and forgets both on Sign out. Cost in
plain words: a credential with no expiry sits in the browser's profile on that device
for as long as the device does; whatever can run script on that origin (the page's own
inline script, under this CSP) or hold the unlocked device can read it; a lost or
borrowed device is `atlas api-key revoke`. Atlas cost: JavaScript and two test sentences;
no C, no migration, no route, no daemon change. **This is the shape the tasks below are
written for**, because it is the only one of the three that fits season A's constraint
list; that is a statement of consequence, not the choice.

**Shape 2 — a login that mints a short-lived submission capability the browser renews
silently.** What renews it must itself be a credential: either the long-lived key (then
nothing was gained) or a refresh token the browser keeps (then a durable secret sits on
the device exactly as in shape 1). The session cookie may not be that credential —
contract §4 says a cookie can never submit, and a cookie that mints a submitting token
submits transitively. Atlas cost: a credential kind with an expiry (none exists — the
key table has no TTL), a table or column and therefore a migration, a mint method the
daemon verifies inside the write transaction, a gateway route, revocation semantics, and
renewal logic in the page. Roughly A16's size. **What it buys is a short life for a token
captured in flight — and the first half of this season is what stops capture in
flight.** Under TLS, shape 2 pays shape 3's machinery for a benefit TLS already
delivered, and it does not bound a stolen device any better than shape 1.

**Shape 3 — a passkey (WebAuthn).** The device's authenticator holds a private key;
the gateway challenges; the browser signs. Verifying that signature is ECDSA over P-256
(or Ed25519/RS256) plus CBOR parsing of the registration attestation, and Atlas has
SHA-256 and HMAC first-party and nothing else: either a first-party elliptic-curve
implementation the project has never carried, or a vendored library with an upstream
tag, digests and a licence entry — a hard-rule-sized cost. Registration is credential
administration, which is absent remotely by design (A9), so the public key would be
enrolled at a terminal. And **per the WebAuthn specification's definition of a valid
RP ID, the relying party is a domain name, not an IP literal** — so a passkey at
`https://192.168.0.198:8799` cannot exist; it needs a hostname, a certificate for that
name, and a DNS or hosts entry on every device first. (This spec constraint is stated
from the specification, not verified on this machine; the Steward's §4 review should
check it. If it is wrong, shape 3's bill loses a hostname and keeps the crypto and the
migration.) It is a season of its own and needs season A's TLS before it can start.

**The login key's alternatives, inside shape 1 (row 3).** Three ways to make the *read*
session survive: (a) remember the login key and re-login silently — the default the
tasks assume; (b) `session_ttl_seconds` up to the ceiling 604800 (one root-owned line,
no code) — but a restart, which every deploy is, still forgets; (c) the anonymous read
floor, built 2026-09-04 for exactly this problem at the Operator's request (reason 569)
and not installed — no login at all for reads, from anyone who can reach the listener,
which behind TLS on the LAN is the LAN. (a) and (c) are the same durability with a
different holder: the device, or the segment.

## The decisions this plan settles, each with its argument

### Decision 1 — No line of C changes; the terminator returns as deployment, the parser's refusal is the guard

The parser already implements the contract's §4 sentence (fact 2), the cookie already
turns `Secure` under the mode (fact 4), and the daemon already offers the groups under
it (fact 3). What was missing was the deployment, which c305f40 wrote and A16 removed
when the Operator declined TLS on 2026-09-04 (*"https ye gerek yok. kendi aginda
sacmalamasin zaten"*, A16 plan row 1) and which the Operator asked for on 2026-09-05
(contract §7, 5.1). **The acceptance keys stay in the parser**: their refusal under
`REVERSE_PROXY` is what fires the day somebody removes the proxy and forgets the line,
and removing the code would be a goal nobody gave (question 1).

### Decision 2 — The acceptance line and the mode change are one edit, and the order of the cut-over is a chain

Fact 2 and fact 3, written as steps in T3. A two-step edit produces a MALFORMED policy
between the steps; a proxy started before the gateway has left the port fails with
"address already in use"; a restart during an active remote job kills it. Each is a
measured or verified failure, and the step order is the only defence.

### Decision 3 — Staging on a second port before the cut-over

The certificate is the one thing this machine cannot verify about the phone. Proving it
on `8443` against the running gateway costs one extra `listen` line and no downtime, and
turns the cut-over's unknown (will the phone trust it?) into a known before anything
moves.

### Decision 4 — `trust_forwarded_for` stays `no`, and the sentences that imply it does something are corrected

Fact 5. The key changes no behaviour today; the limit is global with or without it. The
season deploys exactly the shape those sentences describe, so T1 corrects them to
"behind a reverse proxy every request comes from the proxy; the limit is one fixed
window over the total forwarded rate, and `trust_forwarded_for` changes nothing about
it today" and records the residual in `docs/backlog.md`. No decision row: there is
nothing to decide.

### Decision 5 — The tunnel goes to loopback, not through the proxy

Argued in §The decision on the terminator. One line; no trust question; exposure
unchanged; and `docs/remote-submission.md`'s "does not cross that segment" stays true in
the same words, which the tripwire requires.

### Decision 6 — The page's storage under shape 1: two keys move to `localStorage`, the disposal key does not, Sign out forgets everything

`loginKey` and `submitKey` in `localStorage` under fixed keys; `disposeKey` stays in
`sessionStorage` under A16's ruling unless row 3 moves it; every access wrapped in
`try`/`catch` exactly as the review sheet's is (`mission-control.html:898-925`), so a
private window reads as "nothing remembered", never throws. Sign out calls
`/auth/logout` and clears all three, so "Sign out" and "forget this device" are one
control and one sentence. The remembered submission key is used exactly as the typed
one is today — a bearer on `apiWrite` with `credentials: "omit"` — and never becomes a
cookie.

### Decision 7 — Silent re-login is bounded: one attempt per failed request, never a loop

`api()`'s 401 branch tries `/auth/login` with the remembered key once and retries the
original request once; a second 401 shows the login form with the frozen sentence and
leaves the remembered key in place (a transient failure must not forget it). The
remembered key is cleared only by Sign out or by the operator's explicit "forget".
`apiWrite()`'s 401 is unchanged: it names the bearer it was given, which is the
submission or disposal key and not the session.

### Decision 8 — One new assertion, two places, for a property nothing asserts

`Secure` present after the `/auth/login` `test_c_auth_me_fields` already makes under
`REVERSE_PROXY`, absent after the one it makes under `tls_mode = NONE` with the
acceptance — one widened helper, two checks, in the test whose subject is already what
the policy's TLS stance shows the browser. A season whose whole first half is "the
cookie is now `Secure`" cannot leave that unasserted.

### Decision 9 — The cleartext chains become dated history, closed by a paragraph after them, never deleted

The tripwire requires "does not cross that segment" in three documents (`:497-500`),
and the house habit is to keep an answered row as the record of the answer. Each chain
paragraph gains a dated closing paragraph (§Frozen formats) and loses no sentence.

### Decision 10 — The reference proxy block lives in `deploy/a9/`, named as this plan's one new artefact

c305f40 had the block only inside its task text. A file beside the unit and the template
is what an operator copies; it is named in §File structure as the one thing this season
creates so the Steward can strike it if they read it as scope.

### Decision 11 — The live acceptance spends nothing

Contract §9: no paid model job. T4 proves the credential path over TLS with
`/api/v1/job/list` under the submission bearer and `/auth/me` under the session, and
the tunnel's own readiness; the first paid submission after the move waits for the
Operator's contemporaneous go-ahead, with A14's figures in front of them.

## Frozen formats

### The policy diff (`/etc/atlas/gateway.conf`)

```diff
-# Bound to this machine's LAN address only -- not 0.0.0.0, which would also
-# expose it to the docker0 bridge.
-listen_addr = 192.168.0.198
-listen_port = 8799
-public_url = http://192.168.0.198:8799
+# Season A. The gateway binds loopback; nginx terminates TLS on
+# 192.168.0.198:8799 -- the LAN address only, never 0.0.0.0, which would also
+# expose it to the docker0 bridge -- and forwards here.
+listen_addr = 127.0.0.1
+listen_port = 8787
+public_url = https://192.168.0.198:8799

-# Atlas terminates no TLS. NONE is an explicit statement that this deployment
-# accepts plaintext on a trusted network segment: API keys and session cookies
-# travel in the clear over the LAN.
-tls_mode = NONE
+# Atlas terminates no TLS. REVERSE_PROXY records that nginx, in front on
+# 192.168.0.198:8799, terminates it and forwards over loopback. Atlas cannot
+# verify that and does not try; it is the operator's statement.
+tls_mode = REVERSE_PROXY
```

and, deleted whole — the comment block and the line:

```diff
-# THE OPERATOR'S WRITTEN ACCEPTANCE OF A CLEARTEXT SUBMISSION CHANNEL. ...
-operator_accepts_cleartext_submission = yes
+# The acceptance the operator wrote on 2026-09-04 ("for now") was closed on
+# <T3's date> by TLS in front; the loader refuses that line under REVERSE_PROXY.
```

Every other line is unchanged, including the two `remote_submit_key` lines as they
stand. The previous file is kept as `/etc/atlas/gateway.conf.pre-a-tls` (the
deployment's existing habit: `.pre-a14`, `.pre-tunnel-20260824`).

### The proxy block (`/etc/nginx/sites-available/atlas`, reference copy `deploy/a9/nginx-atlas.conf.example`)

```nginx
# Atlas -- TLS in front of the gateway. A system service the operator runs;
# not an Atlas dependency. Atlas terminates no TLS and cannot verify this file.
server {
    # The address and port the operator's devices already use. The LAN address
    # literally, never 0.0.0.0: that would also expose the listener on docker0.
    listen 192.168.0.198:8799 ssl;
    server_name 192.168.0.198;

    ssl_certificate     /etc/nginx/atlas/atlas.crt;
    ssl_certificate_key /etc/nginx/atlas/atlas.key;
    ssl_protocols       TLSv1.2 TLSv1.3;

    # Equal to the policy's max_request_bytes (1048576) and to Atlas'
    # ATLAS_GW_MAX_BODY_BYTES. Stated so the two bounds are known to agree.
    client_max_body_size 1m;

    location / {
        proxy_pass http://127.0.0.1:8787;
        proxy_http_version 1.1;
        # No `proxy_set_header Host`: nginx's default ($proxy_host, i.e.
        # 127.0.0.1:8787) is what the gateway's host_matches_listener compares
        # against, should the anonymous read floor ever be installed.
        proxy_set_header X-Forwarded-For $remote_addr;
        proxy_set_header X-Forwarded-Proto https;
        # The gateway reads neither header today; they are for this log's sake.
        # Above ATLAS_GW_UPSTREAM_TIMEOUT_MS (60 s), so the gateway's own 504
        # reaches the client rather than nginx's.
        proxy_read_timeout 75s;
    }
}
```

Enabled by `ln -s ../sites-available/atlas /etc/nginx/sites-enabled/atlas`, checked by
`nginx -t`, applied by `nginx -s reload`. During staging (T3 step 3) the same file
reads `listen 192.168.0.198:8443 ssl;` and `proxy_pass http://192.168.0.198:8799;`
and nothing else differs.

### The certificate (root, on this machine)

```sh
install -d -m 0700 /etc/nginx/atlas
# The CA: kept on this machine, installed on the operator's devices, five years.
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 1825 \
  -subj "/CN=Atlas local CA" \
  -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" \
  -keyout /etc/nginx/atlas/ca.key -out /etc/nginx/atlas/ca.crt
# The leaf: the IP as its SAN, serverAuth, at most 825 days -- the published
# ceiling for a leaf a device will trust.
openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
  -subj "/CN=192.168.0.198" -keyout /etc/nginx/atlas/atlas.key -out /etc/nginx/atlas/atlas.csr
printf 'subjectAltName=IP:192.168.0.198\nextendedKeyUsage=serverAuth\nbasicConstraints=CA:FALSE\n' \
  > /etc/nginx/atlas/leaf.ext
openssl x509 -req -in /etc/nginx/atlas/atlas.csr -CA /etc/nginx/atlas/ca.crt \
  -CAkey /etc/nginx/atlas/ca.key -CAcreateserial -days 825 \
  -extfile /etc/nginx/atlas/leaf.ext -out /etc/nginx/atlas/atlas.crt
chmod 0600 /etc/nginx/atlas/ca.key /etc/nginx/atlas/atlas.key
openssl x509 -in /etc/nginx/atlas/ca.crt -noout -fingerprint -sha256 -enddate
openssl x509 -in /etc/nginx/atlas/atlas.crt -noout -fingerprint -sha256 -enddate
```

Both fingerprints and both `notAfter` dates are recorded in the deployment section of
`docs/remote-access.md` as observations. `ca.crt` is what each device installs; the
device steps — iOS: install the profile, then Settings → General → About → Certificate
Trust Settings, full trust; Android: Settings → Security → install a CA certificate,
honoured by Chrome for pages but not by apps; a desktop browser: its own trust store —
are **published requirements T4 records against, not facts this machine can verify.**

### The tunnel line (`~/.config/tunnel-client/atlas.yaml:22`)

```yaml
      url: "http://127.0.0.1:8787/mcp"
```

Then `systemctl restart atlas-tunnel.service` (root; the unit runs as `nocdem`).
Nothing else in the file changes; the `Authorization: "env:ATLAS_AUTH_HEADER"` lines are
untouched and their value is never read by anyone executing this plan.

### `atlas gateway status` after T3

```
gateway: ENABLED
reason:  ACTIVE
policy:  /etc/atlas/gateway.conf
listen:  127.0.0.1 port 8787
tls:     REVERSE_PROXY (Atlas terminates no TLS)
surface: remote_mcp=yes web_gui=yes
uid:     992
origins: 0 allowed
anon:    (none -- /api/ still requires a session or bearer credential)
dispose: (none -- the browser can read and queue, never dispose)
clear:   (not accepted -- a disposal credential is offered only behind tls_mode = REVERSE_PROXY)
submit:  key_b2578f48143c06d3 key_1515cefd1f9f71a4  (driver claude, mode patch, 1 gate(s), attempts 1, active 2, per day 6; checked at submit)
clear-submit: (not accepted -- a submission credential is offered only behind tls_mode = REVERSE_PROXY)
url:     https://192.168.0.198:8799/mcp
```

The `clear:` and `clear-submit:` wording under `REVERSE_PROXY` is question 5; this plan
changes no renderer text.

### The page under shape 1 (`src/gw/ui/mission-control.html`)

Storage keys, all in one block beside the existing two:

```js
const LOGIN_KEY_STORAGE_KEY  = "atlas.login.key.v1";   /* localStorage */
const SUBMIT_KEY_STORAGE_KEY = "atlas.submit.key.v1";  /* localStorage (was sessionStorage) */
const DISPOSE_KEY_STORAGE_KEY = "atlas.dispose.key.v1"; /* sessionStorage, unchanged (A16 row 3) */
```

`loginKeyLoad` / `loginKeySave` / `loginKeyClear` and the submit-key trio read and write
`localStorage`, wrapped exactly as `reviewQueueLoad` / `reviewQueueSave` are. The
sign-in handler saves the key **after** `/auth/login` answers 200 and still clears the
input. `api()`'s 401 branch: if `loginKey` is non-empty, one `POST /auth/login`, then
one retry of the original request; on a second 401, `showLogin()` with the sentence
below. Sign out: `/auth/logout`, then `loginKeyClear()`, `submitKeyClear()`,
`disposeKeyClear()`, then `start()`.

The fixed sentences, byte-for-byte (the test binds on the quoted fragments):

- Sign-in panel, under the key field:
  `"Signing in remembers this key on this device until you sign out. Anyone who can use this browser can then read what it reads; a lost device is a revocation, not a password change."`
- Jobs view, replacing `:1992-1993`:
  `"The submission key is remembered on this device until you sign out; anyone who can use this browser can queue work with it, and a lost device is a revocation with atlas api-key revoke."`
- After a failed silent re-login (`#loginerr`):
  `"The remembered key was not accepted; sign in again."`
- The disposal panel's sentence at `:1383-1384` is **unchanged** unless row 3 moves the
  disposal key, in which case it becomes the Jobs view's sentence with "disposal" for
  "submission" and "dispose of records" for "queue work".
- The `#signout` button's `title`: `"Also forgets the keys remembered on this device."`
- The comment at `:2280-2283` is rewritten to say what is now true: the key is stored,
  where, and that Sign out is what forgets it.

### The test bindings (`tests/test_gw_remote.c`)

In `test_mission_control_carries_the_jobs_view` (`:2825`), `BOUND[]` gains:
`"atlas.login.key.v1"`, `"localStorage.getItem(SUBMIT_KEY_STORAGE_KEY)"`,
`"remembered on this device until you sign out"`, `"remembers this key on this device"`,
`"The remembered key was not accepted"`; and a forbidden check that
`"The submission key is remembered for this tab only"` is absent (the disposal sentence
keeps its own `"remembered for this tab only"`, which `test_mission_control_carries_the_disposal_panel`
at `:2988` still requires).

**The `Secure` assertion (`tests/test_gw_submit.c`).** `login_and_get_cookie`
(`:370-383`) gains a last parameter `bool *secure_out` (NULL allowed), set from
`strstr(<the Set-Cookie line>, "; Secure") != NULL` before the response is freed; every
existing caller passes NULL. In `test_c_auth_me_fields` (`:864`): instance 1
(`REVERSE_PROXY`, login at `:892`) asserts `secure == true`; instance 3 (`tls_mode =
NONE` with the acceptance, login at `:965`) asserts `secure == false`. Each with a
one-line comment naming `src/gw/gateway.c:1938-1950` and saying this is the property
season A leans on.

### The closing paragraph (dated, after each cleartext chain)

Placed after `docs/remote-submission.md:270-291`, `docs/remote-access.md:566-576`,
`SECURITY.md:622-633` and, for the disposal chain, `SECURITY.md:596-603` and
`docs/browser-disposal.md:37-56`, with the date T3 records:

> **Closed <date>.** The gateway now listens on `127.0.0.1:8787` under `tls_mode =
> REVERSE_PROXY`, behind nginx on `192.168.0.198:8799` terminating TLS with a
> certificate the operator's devices were told to trust; `operator_accepts_cleartext_submission`
> came out of the policy in the same edit, and the loader would refuse it under this
> mode. The chain above is kept as the record of what was accepted between 2026-09-04
> and that date. What did not change: the credential the MCP tunnel presents does not
> cross that segment — it now posts to the gateway's loopback listener — and its
> exposure is still the file it is read from; a named credential still has no expiry
> and still queues work that runs as the operator until it is revoked; Atlas still
> terminates no TLS and cannot verify what is in front of it. On a device that has not
> installed the CA, the browser's warning is the only thing between the operator and a
> listener that is not this one.

### The corrected rate-limiting sentence (three documents, two headers)

> The limit is one fixed window over the gateway's total forwarded request rate. Behind
> a reverse proxy every request comes from the proxy, and the limit is the same global
> one it always was; `trust_forwarded_for` is parsed and printed and changes nothing
> about it today — no code reads a forwarded address. It stays `no`, and the residual is
> in `docs/backlog.md`.

### The season rules for `CLAUDE.md`

```
### Season A — the channel and the sign-in

- **AN ACCEPTANCE GIVEN "FOR NOW" HAS A CLOSING STEP, AND THIS IS IT.** The
  operator wrote `operator_accepts_cleartext_submission = yes` on 2026-09-04
  with the intent to put TLS in front later; the line came out on <date> in
  the same edit that set `tls_mode = REVERSE_PROXY`, because the loader
  refuses the pair.
- **Atlas terminates no TLS, and no line of C changed to put nginx in front.**
  The terminator is a system service the operator runs; `deploy/a9/` carries
  an example block, not an installation.
- **The acceptance keys stay in the parser.** Their refusal under
  `REVERSE_PROXY` is the guard that fires the day the proxy comes out and the
  line is forgotten.
- **A CREDENTIAL REMEMBERED ON A DEVICE IS EXACTLY AS DURABLE AS THE
  CREDENTIAL, AND THE PAGE SAYS SO.** A remembered key stores no authority the
  credential did not already hold; Sign out forgets every remembered key; the
  disposal key stays per-tab under A16's ruling unless the operator moves it;
  a session cookie still submits and disposes of nothing.
- **`trust_forwarded_for` changes nothing today.** The limit is one global
  fixed window with or without it, and the documents now say so.
- **The tunnel posts to loopback.** Its credential did not cross the segment
  before and does not now.
- **No new thread, process, timer, migration, route, scope, daemon method or
  MCP tool; no header interface moves (two header comments are corrected);
  `atlas_decision_apply_in_tx` keeps three callers and `atlas_orch_apply_in_tx`
  one.**
```

## Authority argument — the season's non-negotiables

1. **What the gateway cannot do stays true because of who it runs as.** nginx in front
   adds a process that sees every byte of every request, including bearer credentials,
   and runs as `www-data` (`/etc/nginx/nginx.conf:1`, `user www-data;`) with access to
   the private key — a second principal in the
   path, stated as such. It cannot reach the socket (`atlas-clients` group) and holds no
   Atlas authority; what it can do is impersonate the gateway to any device that trusts
   its CA, which is the CA key's cost (§Worst-case cost).
2. **Authority stays outside the principal it constrains.** The policy, the proxy block
   and the certificate are root-owned; the gateway's unit keeps `ReadOnlyPaths=/etc/atlas`
   and cannot write the file that now says `REVERSE_PROXY`.
3. **A session cookie can never submit or dispose**, under every row 2 answer: the
   cookie's routes do not change; the remembered submission key is a bearer on
   `apiWrite` with `credentials: "omit"`, never a cookie; the login key's session is
   read-scoped as before.
4. **The daemon's offer predicates do not move.** `REVERSE_PROXY` satisfies them as the
   preferred shape (`server_orch_remote.c:70-71`, `server_remote.c:132`); the
   acceptance branch is simply no longer taken on this deployment.
5. **`REMOTE_OPERATOR_CONFIRMED` and `LOCAL_OPERATOR_CONFIRMED` are unchanged**, and no
   sentence this season writes claims a channel establishes that a person acted; the
   remembered-key sentences name the device and the browser, not a person.
6. **The caller counts are unchanged and the tripwires prove it**
   (`tests/test_decision_mcp.c:582`, `tests/test_review_apply.c:1442`).

## Worst-case cost, stated so nobody discovers it in a bill

**Model money in this plan: $0.** T4 sends no paid job (contract §9). The first paid
submission after the move is the Operator's call, with A14's figures in front of them:
**$5.13 per worker start observed** (`docs/roadmap.md:979`), about **$10.6 at the 900 s
wall bound, extrapolated** and labelled as such, `keys × per_day × attempts ×
cost-per-start` at the policy's lines — two keys, six a day, one attempt: $61.6 observed
rate, $127 at the bound, per UTC day, unchanged by this season.

**Executor and review tokens.** Three code/docs dispatches (T1, T2, T5 — Sonnet), one
review (Opus, T2 only: the credential-storage change is the trust-boundary edit; T1
and T5 close on tests and a diff read, per the Cost rules). T3 and T4 are the Operator's
time, not tokens.

**The Operator's time.** T3: about fifteen minutes at the machine as root, of which the
window with nothing answering on the LAN address is the two restarts. Per device: the
CA install and trust steps, a few minutes each; the device list is row 1's second half.

**Downtime.** Fact 11's hazard is avoided by checking the remote job list first. The
tunnel is down from the gateway restart until its own restart.

**Ongoing.** One more service in the path: if nginx stops, Mission Control and the
tunnel are unreachable while the gateway is healthy, and `atlas gateway status` cannot
say so — it reports the policy, not the proxy. The leaf certificate expires **825 days
after issue**, an outage with a date, written into the deployment record so nobody
discovers it on the day. The CA private key on this machine (root, 0600): whoever reads
it can present a certificate every trusting device accepts, for any name, for five
years; stated, not solved. nginx's access log records request lines — repository
names, decision ids, never the `Authorization` header by default — a second log on the
machine holding request paths.

**What this season does not reduce.** A named credential's value once presented; a
stolen device under shape 1; the transcript chain A14 states; anything about what a
worker may read.

---

# Decisions the operator must be asked, and when — read this before dispatching anything

A15 left a choice in a document and fourteen hours followed; A16 and A14 asked their
rows before T1. Every choice this plan leaves is here, in plain words about the thing
itself, with what the tasks below assume and **the task before which it must be
asked**. Contract §9: these rows wait for the Operator; the delegation covers the plan's
approval and not its questions.

| # | Question, in full | What the tasks assume | Ask before |
| --- | --- | --- | --- |
| 1 | **Which certificate, and which devices?** *(a)* A private CA made on this machine, installed as trusted on each device you will use, signing a leaf whose name is `192.168.0.198` — free, no DNS, a trust step per device, and a warning on any device that skipped it. *(b)* A public hostname you own, resolving to `192.168.0.198` on your network, with a publicly trusted certificate — no per-device step, but a domain, a DNS entry your devices resolve, and issuance by DNS-01 because apache holds port 80 for another site; renewal becomes a scheduled job. Either way Atlas terminates none of it. **And the device list**: which phones, tablets and browsers will trust the CA under (a)? T4 records each one by name. You answered no to TLS on 2026-09-04 (*"https ye gerek yok"*) and yes on 2026-09-05 (contract §7, 5.1); this row asks only the shape. | (a); the phone plus whichever browsers you name | **T3** |
| 2 | **Which sign-in shape?** *(1)* Remember on the device: the page keeps the keys in the browser's storage for `https://192.168.0.198:8799` until you sign out, and signs the session back in silently after a restart — a key with no expiry sits on the device; a lost or borrowed device is `atlas api-key revoke`; JavaScript only. *(2)* A login that mints a short-lived submission token the browser renews: needs a credential kind with an expiry, a migration, a daemon method verified in the write transaction, a route, and something durable on the device to renew with — about A16's size — and it buys a short life for a token captured in flight, which TLS already prevents. *(3)* A passkey: your device's Face ID / fingerprint signs a challenge; Atlas would need elliptic-curve signature verification it does not have (first-party crypto or a vendored library under the hard rules), enrolment at a terminal, and — per the WebAuthn specification — a hostname rather than an IP, so a domain and a certificate for it first; a season of its own, after this one. **The tasks below are written for (1); under (2) or (3) this plan is re-issued, not amended**, because the file structure and the bill change. | (1) | **T2** |
| 3 | **Which keys are remembered, and how does the read session survive?** Under shape 1: the submission key — yes, that is the row's premise. The **login key**: *(a)* remembered too, with silent re-login (the tasks assume this); *(b)* not remembered, `session_ttl_seconds` raised toward its ceiling (604800 = a week; one root-owned line) — a restart, which every deploy is, still asks again; *(c)* the anonymous read floor, built on 2026-09-04 at your request for exactly this restart problem and never installed — no login for reads, from anyone who can reach the listener, which behind TLS on your LAN is your LAN. The **disposal key**, when one is installed: A16's per-tab ruling stands unless you move it here; moving it means a captured device disposes of records of the policy's kinds until revoked. **Remember by default after the first sign-in** (your words: *"bir kez girdikten sonra"*), with Sign out as the way to forget — or an explicit "remember on this device" box at sign-in? | login key (a); disposal key unchanged; remember by default | **T2** |

Row 4 of c305f40 (`X-Forwarded-For`) is struck by fact 5; row 5 (the anonymous floor
behind the proxy) is folded into row 3 (c).

---

# File structure

**Create:**

| Path | Responsibility |
| --- | --- |
| `deploy/a9/nginx-atlas.conf.example` | the proxy block, verbatim from §Frozen formats — **this plan's one new artefact**, named so the Steward can strike it |
| `docs/plans/2026-09-05-a-channel-and-signin.md` | this plan |

**Modify (repository):** `src/gw/ui/mission-control.html` (T2); `tests/test_gw_submit.c`
(T1); `tests/test_gw_remote.c` (T2); `SECURITY.md`, `docs/remote-access.md`, `include/atlas/limits.h:1005-1008`
and `include/atlas/gwpolicy.h:296-303` (T1, the rate-limiting sentence — comments
only, no interface; the index's 14 dependents recompile and nothing else moves);
`docs/remote-submission.md`, `docs/browser-disposal.md`, `README.md:459`, `CLAUDE.md`
(opening paragraph, the season table, the rules block), `docs/roadmap.md` (a season A
section before A16's, and `:1630-1631`'s "today this listener is cleartext" dated),
`docs/engineering-rules.md` (the season's rules in full with the chains above),
`docs/backlog.md` (§Underspecified 4 and 5, the inert key, the proxy-hostname key
`gateway.c:530-547` declines, the passkey's hostname prerequisite),
`deploy/a9/gateway.conf.template:142` (one sentence at `trust_forwarded_for`),
`include/atlas/atlas.h:11` (`ATLAS_PHASE`, per question 2) — all T5.

**Modify (this machine, root, T3):** `/etc/atlas/gateway.conf` (§Frozen formats, with a
`.pre-a-tls` copy); `/etc/nginx/sites-available/atlas` + the `sites-enabled` link;
`/etc/nginx/atlas/` (the CA, the leaf, both keys 0600); `~/.config/tunnel-client/atlas.yaml:22`;
`/opt/atlas/deploy.local.sh:17` (`MARK='atlas.login.key.v1'`, a string only the T2
binary's embedded page contains; gitignored).

**Deliberately not modified:** `src/gw/gateway.c`, `src/gw/gwpolicy.c`, `src/gw/http.c`
(no behaviour changes; Decision 1 and fact 5); `src/ipc/server_orch_remote.c`,
`src/ipc/server_remote.c`, `src/daemon/daemon.c` (the predicates already prefer this
mode); `tests/test_gateway.c` (the matrix already covers the acceptance-under-
`REVERSE_PROXY` refusals at `:184-190`); `deploy/a9/atlas-gateway.service` and its
installed copy (question 4); apache2 and everything under `/etc/apache2/`; the
`remote_submit_*` lines and both submission keys; `~/.config/tunnel-client/atlas.env`;
the disposal-panel sentence unless row 3 moves it; `src/mcp/*`, `src/decision/*`,
`src/orch/*`.

---

# Tasks

Dependency order: **T1 ∥ T2** → T3 → T4 → T5. T1 needs no row and is the only task §9
lets the autonomous trial dispatch; T2 waits for rows 2 and 3 and goes out with T1 in
one dispatch once they are answered; T3 needs rows 1 and 3, the Operator at the machine,
and the T1+T2 binary built; T4 needs T3 and the Operator's devices; T5 states what T3
and T4 observed, dated.

---

### Task T1: the `Secure` assertions, the reference proxy block, and the sentence that implied a per-peer limit

**Files:**
- Modify: `tests/test_gw_submit.c`, `SECURITY.md:501-504`, `docs/remote-access.md:666-676`,
  `include/atlas/limits.h:1005-1008` (comment), `include/atlas/gwpolicy.h:296-303`
  (comment), `docs/backlog.md`
- Create: `deploy/a9/nginx-atlas.conf.example`

**Needs no operator row. Dispatchable under §9.** The reference block is written for
row 1's (a); under (b) exactly its two `ssl_certificate*` lines change to the public
certificate's paths, which T3 edits on the machine and T5 mirrors in the example —
nothing in T1 waits on that answer. Model: Sonnet. Review: none — tests and a diff
read close it (nothing here touches authority, a bound or a refusal).

- [ ] **Step 0: Re-read.** Read every line reference in this task against `HEAD` and
      amend this document, dated, where a number moved.
- [ ] **Step 1: Write the assertions.** Exactly as §Frozen formats → The `Secure`
      assertion: widen `login_and_get_cookie` (`tests/test_gw_submit.c:370-383`) with
      `bool *secure_out`; in `test_c_auth_me_fields` (`:864`) assert `true` after the
      instance 1 login (`:892`, `REVERSE_PROXY`) and `false` after the instance 3 login
      (`:965`, `tls_mode = NONE` with the acceptance). Every other caller of the helper
      passes NULL.
- [ ] **Step 2: Run `test_gw_submit` and watch both assertions pass** — both describe
      existing behaviour; the point is that nothing asserted either before. If either
      fails, stop: the fact in §What exists 4 is wrong and this plan is amended before
      anything else.
- [ ] **Step 3: The reference block.** Create `deploy/a9/nginx-atlas.conf.example` with
      the block from §Frozen formats, verbatim, comments included.
- [ ] **Step 4: The sentence.** Replace the "unless `trust_forwarded_for` is set"
      sentences in the four places named with the corrected sentence from §Frozen
      formats (in the two headers, as the comment's wording; the field and the parser
      are untouched). Add a `docs/backlog.md` entry: `trust_forwarded_for` is parsed and
      inert; a per-peer limit behind a proxy needs a reader of the forwarded address and
      a per-peer table, neither of which exists.
- [ ] **Step 5: Run `test_gw_submit`, `test_gw_remote`, `test_gateway`,
      `test_decision_mcp`**; `make` with zero warnings.
- [ ] **Step 6: Record a change reason in Atlas** (the two assertions, the example
      block, the corrected sentence) — or `UNKNOWN`.
- [ ] **Step 7: Commit** — `test(<season>): the session cookie is Secure exactly under REVERSE_PROXY, asserted for the first time; the proxy block Atlas never installs; a key that changed nothing said so`

---

### Task T2: Mission Control remembers — shape 1

**Files:**
- Modify: `src/gw/ui/mission-control.html`, `tests/test_gw_remote.c`

**Waits for rows 2 and 3.** If row 2 is not (1), this task and the plan are re-issued.
Model: Sonnet. **Review: Opus, one pass** — this is the season's trust-boundary edit
(the honest paragraph), and the named suspicion is a re-login loop or a key that
survives Sign out.

- [ ] **Step 0: Re-read** `mission-control.html:344-411`, `:445`, `:464-488`,
      `:898-925`, `:1383-1384`, `:1949-1960`, `:1992-1993`, `:2190-2211`, `:2233-2245`,
      `:2275-2287` against `HEAD`; amend this document where a line moved.
- [ ] **Step 1: Write the failing test.** Extend `BOUND[]` in
      `test_mission_control_carries_the_jobs_view` with the five fragments in §Frozen
      formats → The test bindings, and add the forbidden check for the old submission
      sentence. Run; watch the new fragments fail.
- [ ] **Step 2: Implement.** The three storage keys and the trio of functions for the
      login key; the submission trio moved to `localStorage`; the disposal trio
      untouched (or moved, if row 3 said so, with its sentence rewritten as §Frozen
      formats says). The sign-in handler saves after 200. `api()`'s bounded re-login
      (Decision 7). Sign out clears all three then calls `start()`. The three fixed
      sentences and the `#signout` title, byte-for-byte. The comment at `:2280-2283`
      rewritten to what is now true. Every storage access wrapped in `try`/`catch`.
      `textContent` only — the four markup sinks the test already forbids stay absent.
- [ ] **Step 3: Run `test_gw_remote`, `test_decision_mcp`** (the page is in its
      `FILES[]`; the new sentences must trip none of the fourteen phrasings and must
      keep `"names the channel, not a person"`, `"stores no authority"`,
      `"records the channel and the credential, not a person"`, `"a task is a prompt"`
      present); `make` with zero warnings.
- [ ] **Step 4: Build the release binary** (`make`) so T3 has one binary to deploy;
      confirm `strings build/atlas | grep -c atlas.login.key.v1` is non-zero — that is
      the `MARK` T3 uses.
- [ ] **Step 5: Record a change reason in Atlas**, or `UNKNOWN`.
- [ ] **Step 6: Commit** — `feat(<season>): Mission Control remembers the keys it is given on this device, signs the session back in itself, and forgets everything on Sign out`

---

### Task T3: the cut-over on this machine — staged, then moved, in the order that cannot fail halfway

**Run by the Operator as root, with the reviewing session reading each step's output.
Deploy under contract §9: not the trial's to run. Spends no money. Needs rows 1 and 3
answered and the T1+T2 binary built.**

- [ ] **Step 1: Deploy the binary.** Set `MARK='atlas.login.key.v1'` in
      `/opt/atlas/deploy.local.sh:17`; run it; the daemon restarts and pings. (The
      gateway is restarted in step 5, not here.) **Before running it: `atlas job list
      --remote` shows no non-terminal job** — fact 11; if one is active, wait.
- [ ] **Step 2: The certificate.** The commands in §Frozen formats → The certificate.
      Record both fingerprints and both `notAfter` dates. Install `ca.crt` on each device
      row 1 named, following the published steps; record what each device did.
- [ ] **Step 3: Stage the proxy.** `/etc/nginx/sites-available/atlas` as §Frozen formats
      with the staging `listen` (`8443`) and `proxy_pass` (`http://192.168.0.198:8799`);
      link it; `nginx -t`; `nginx -s reload`. On the phone: `https://192.168.0.198:8443/`
      opens Mission Control **without a certificate warning** and a sign-in works. On
      this machine: `curl -s --cacert /etc/nginx/atlas/ca.crt https://192.168.0.198:8443/healthz`
      answers `{"ok":true}`. If the phone warns, stop here: nothing has moved, and the
      certificate is what needs fixing. (The `:8443` origin's browser storage is
      orphaned at the cut-over exactly as the `http://` origin's is; a sign-in made here
      is a certificate check, not the sign-in T4 observes.)
- [ ] **Step 4: Save what the move orphans.** Any queued review sheet in the browser at
      the `http://` origin is saved as a file or applied with `atlas review apply`
      (§What breaks, 3). `cp /etc/atlas/gateway.conf /etc/atlas/gateway.conf.pre-a-tls`.
- [ ] **Step 5: The policy and both restarts — the one edit.** Apply §Frozen formats →
      The policy diff in a single write: listener, `public_url`, `tls_mode`, **and the
      acceptance block removed**. `atlas gateway status` reads exactly the block in
      §Frozen formats (`listen: 127.0.0.1 port 8787`, `tls: REVERSE_PROXY`,
      `clear-submit: (not accepted -- ...)`). Then `systemctl restart atlas.service`
      and `systemctl restart atlas-gateway.service`; `atlas daemon ping`;
      `ss -ltnp | grep 8787` shows the gateway on `127.0.0.1:8787` and nothing on
      `8799`. From this restart until step 6's reload, nothing answers on the LAN
      address; the Operator is told so before step 5 runs.
- [ ] **Step 6: Move the proxy onto the address.** Edit the block to its final
      `listen 192.168.0.198:8799 ssl;` and `proxy_pass http://127.0.0.1:8787;`;
      `nginx -t`; `nginx -s reload`. `curl -s --cacert ... https://192.168.0.198:8799/healthz`
      answers `{"ok":true}`; `curl -s http://192.168.0.198:8799/` answers **nginx's own
      400 for plain HTTP on a TLS port, never Atlas' page**; `curl -s http://192.168.0.198:8787/`
      is refused outright.
- [ ] **Step 7: The tunnel.** Edit `~/.config/tunnel-client/atlas.yaml:22` to
      `url: "http://127.0.0.1:8787/mcp"`; `systemctl restart atlas-tunnel.service`;
      `curl -s http://127.0.0.1:8080/readyz` and `journalctl -u atlas-tunnel -n 20`
      show it connected. Nothing else in that directory is opened.
- [ ] **Step 8: Verify and record**, each as an observation with the date and the
      device: the status block; the two `curl` refusals; the phone opening
      `https://192.168.0.198:8799/` without a warning; `journalctl -u atlas` showing the
      daemon offering the remote-submit group under `REVERSE_PROXY`. Record the nginx
      block and the policy diff (key ids to their first four hex) in
      `docs/remote-access.md`'s deployment section — T5 writes the prose; this step
      keeps the raw observations.
- [ ] **Step 9: Nothing is committed from this task**; `deploy.local.sh` is gitignored.

---

### Task T4: live acceptance — the channel and the sign-in, observed, with nothing spent

**With the Operator's devices. Sends no paid job (contract §9).**

- [ ] **Step 1: Sign in once.** On the phone, `https://192.168.0.198:8799/`, sign in
      with the `mission-control` key; the sign-in sentence is on screen. Close the
      browser entirely; reopen the URL: **no login form** (the remembered key signed the
      session back in). Record it.
- [ ] **Step 2: Survive a restart.** `systemctl restart atlas-gateway.service` on the
      machine (sessions forgotten); reload the page on the phone: no login form. Record
      it — this is the case reason 569 built the floor for, answered without the floor.
- [ ] **Step 3: The submission key, remembered.** Jobs view: paste the browser's
      submission key once; the Jobs sentence is on screen; close and reopen the browser;
      the key field is filled. Then the read that proves the credential path over TLS:
      the job list loads (that is `/api/v1/job/list` under the bearer, through nginx, to
      the daemon, verified by the daemon). **No `Submit`.** Record it.
- [ ] **Step 4: Sign out forgets.** Sign out; reopen: the login form, and the Jobs key
      field empty. Record it.
- [ ] **Step 5: The tunnel end to end.** From the external model, one read-only Atlas
      tool call over `/mcp` (a repository overview or a decision list — never
      `atlas_job_submit`); the Audit view shows the `REMOTE_MCP` row. Record it.
- [ ] **Step 6: Record every observation** in `docs/remote-access.md`'s deployment
      section with the date and the device, and anything the page did that the grep
      test could not have seen. **Nothing about a live pass is a general result; say so
      in those words.**
- [ ] **Step 7: The paid submission is not this task.** If the Operator wants one, it is
      their contemporaneous go-ahead with the §Worst-case figures in front of them, and
      the result goes in the same section.

---

### Task T5: the documents, the season rules, the roadmap — written from what T3 and T4 observed

**Files:** `docs/remote-access.md`, `docs/remote-submission.md`, `docs/browser-disposal.md`,
`SECURITY.md`, `README.md`, `CLAUDE.md`, `docs/roadmap.md`, `docs/engineering-rules.md`,
`docs/backlog.md`, `deploy/a9/gateway.conf.template`, `include/atlas/atlas.h`.

Model: Sonnet. Review: none — `test_decision_mcp` scans every one of these and a diff
read closes it.

- [ ] **Step 1: `docs/remote-access.md`.** §TLS (`:86-100`) gains the deployment
      subsection: the shape (nginx on `192.168.0.198:8799`, the gateway on
      `127.0.0.1:8787`, the CA and leaf with their fingerprints and expiry dates, the
      device list), what the proxy forwards and what the gateway does with it, the
      tunnel on loopback, the two `curl` refusals as observations, and "What breaks
      while the listener moves" as a dated record. The closing paragraph after the
      cleartext chain at `:566-576`; the disposal-acceptance section at `:102-135`
      gains one sentence: not present on this deployment as of the date. §Sessions and
      the page: the remembered keys, their sentences, and the honest paragraph. The
      corrected rate-limiting sentence is T1's; confirm it is there.
- [ ] **Step 2: `docs/remote-submission.md`.** The closing paragraph after §5; §7 gains
      the date the acceptance closed and the key-id drift (fact 1) dated; §6 records T4.
      `docs/browser-disposal.md`: one dated sentence after `:37-56` (no disposal key on
      this deployment today; when one is installed it is offered under `REVERSE_PROXY`
      with no acceptance line) and the closing paragraph. `SECURITY.md`: the closing
      paragraph after `:622-633` and after `:596-603`; the anonymous-floor chain at
      `:540-550` gains "the listener is no longer cleartext as of <date>; the floor is
      still not installed". `README.md:459`: the chain is closed, dated.
- [ ] **Step 3: `CLAUDE.md`.** The opening paragraph names season A with its sentence,
      "no migration", and the season document (`docs/remote-access.md` §TLS); the
      season table gains its row; the rules block from §Frozen formats goes above
      `### A14`. `docs/engineering-rules.md`: the same rules with the chains from
      §Design. `docs/roadmap.md`: a "Season A — the channel and the sign-in (shipped)"
      section above A16's, with the Operator's two answers quoted and dated;
      `:1630-1631` amended to "this listener was cleartext until <date>".
- [ ] **Step 4: The residuals** in `docs/backlog.md`: questions 4 and 5 as the Operator
      answered them or left them; the proxy-hostname key `gateway.c:530-547` declines;
      the passkey's hostname prerequisite; `atlas gateway status` reporting the policy
      and not the proxy. `deploy/a9/gateway.conf.template:142`: one sentence — inert
      today.
- [ ] **Step 5: `include/atlas/atlas.h:11`** per question 2; `atlas daemon ping --json`
      reports it after the next deploy (which is the Operator's).
- [ ] **Step 6: Run `test_decision_mcp`** (every file above is in `FILES[]` or carries a
      required needle), `test_plugin`, then `ctest -LE daemon` and `ctest -L daemon`.
- [ ] **Step 7: Record a change reason in Atlas**, or `UNKNOWN`.
- [ ] **Step 8: Commit** — `docs(<season>): the acceptance given "for now" is closed, the terminator is a system service Atlas never installs, and a remembered key is worth exactly what the credential is`. **Nothing is pushed on this document's authority.**

---

# Acceptance — the contract's requirements, mapped

| # | Requirement (contract §7 5.1, §4, the brief) | Discharged by | The assertion that proves it |
| --- | --- | --- | --- |
| 1 | TLS in front; Atlas terminates none and is never described as doing so | Decision 1; T3; every document sentence | `tls: REVERSE_PROXY (Atlas terminates no TLS)` on the machine; `curl http://…:8799/` gets nginx's 400, never Atlas; `test_decision_mcp`'s scan of every document this season writes |
| 2 | the cleartext acceptance out of the policy, because there is then nothing to accept | fact 2; T3 step 5 | `clear-submit: (not accepted -- …)` after the edit; `tests/test_gateway.c:184-190` proves the loader refuses the line under the mode; the disposal acceptance is absent before and after |
| 3 | the parser's behaviour with the keys absent under `REVERSE_PROXY`, checked | fact 2, fact 3 | `gwpolicy.c:914-921`, `:828-838`; `server_orch_remote.c:70-71`; a loopback bind with `REVERSE_PROXY` is ENABLED (the matrix's base texts at `test_gateway.c:122-167`) |
| 4 | sign-in once, remembered by the browser; the shape the Operator's decision | §The decision on the sign-in; row 2; T2; T4 steps 1–4 | the five bound fragments; `Secure` under the mode; no login form after a browser close and after a gateway restart; Sign out forgets |
| 5 | A16's `sessionStorage` ruling for the disposal key stands unless moved | Decision 6; row 3 | the disposal test's `"remembered for this tab only"` still required; the key's trio untouched by default |
| 6 | the tunnel re-pointed; what breaks while that happens | Decision 5; §What breaks; T3 steps 5–7; T4 step 5 | the one `url:` line; `/readyz`; a read-only tool call over `/mcp` after the move |
| 7 | the gateway holds no authority of its own; authority configured outside its reach | §Authority argument 1–2 | no route, scope, method or tool added; the policy and the proxy are root-owned; the unit's write paths unchanged |
| 8 | a session cookie can never submit or dispose | §Authority argument 3 | the bearer-only table unchanged; the remembered submission key travels as a bearer with `credentials: "omit"` |
| 9 | the two caller counts unchanged; no thread, process, timer, loop | §Global constraints | the two tripwires at `test_decision_mcp.c:582` and `test_review_apply.c:1442`; no C in the diff |
| 10 | the Operator asked before, not after; the plan grounded in the index | §Decisions; §What Atlas answered | three rows, each before the task that needs it; the index's answers cited with their generation |
| 11 | worst-case cost, in numbers; nothing paid in the trial | §Worst-case cost; Decision 11 | $0 in this plan; A14's per-start figures carried; T4 sends no job |
| 12 | §9's edges respected | §What §9 permits | T1 alone dispatchable; T3 is the Operator's; nothing pushed |

---

# Self-review (planner)

**1. Spec coverage.** Every deliverable in the brief is a row of the acceptance table.
The contract's §4 constraints are the six non-negotiables. §9 has its own section
because it decides what can actually happen this week. §10 is honoured as the
coordinator narrowed it: the index was asked and its answers are cited; a dependency is
named for C; nothing was written into it.

**2. Sentences of the recurring defect class, hunted.** Every line reference was read
at `712c9a2` this session and confirmed unchanged at `81e71b1`. Five things this plan
found that the brief did not carry: nginx is already installed and idle and apache
owns 80/443 for another site, so the terminator is a block and not an install; no
Let's Encrypt certificate exists; `trust_forwarded_for` is consumed by nothing; no
test asserts the `Secure` attribute; the disposal acceptance is not in the live policy
and the second submission key's id differs from the one the season document names.
Two things this plan states as unverified rather than as facts: every device-side
certificate behaviour, and the WebAuthn RP-ID constraint that makes shape 3 need a
hostname.

**3. Placeholder scan.** Two placeholders exist and are named: `<date>` in the closing
paragraphs and the rules block, filled by T5 from T3's record because a date written
before the cut-over would be a false sentence; and `<season>` in the commit prefixes,
which is question 2. Nothing else is left open: every policy line, the proxy block, the
certificate commands, the tunnel line, the storage keys, every fixed sentence, every
test binding and every status line is written out.

**4. Type consistency.** `LOGIN_KEY_STORAGE_KEY` is the string in the page, in
`BOUND[]`, and in `deploy.local.sh`'s `MARK`; `test_c_auth_me_fields`'s instance 1
(`REVERSE_PROXY`) is where `Secure` is asserted present and its instance 3 (`NONE` with
the acceptance) where it is asserted absent, through one widened helper;
`127.0.0.1:8787` is the policy's listener, the proxy's
`proxy_pass`, the tunnel's `url:`, and the `Host` `host_matches_listener` would compare
against; `1m` and `1048576` and `ATLAS_GW_MAX_BODY_BYTES` are one number.

**5. What this plan does not settle, and says so.** Whether the acceptance keys should
leave the parser (question 1); the season's name (2); `IPAddressDeny` (4); the status
wording (5); a proxy-hostname key for the floor; a per-peer limit; the CA key's custody
beyond "root, 0600"; the certificate's renewal in 825 days; and every device the
Operator does not name in row 1.

---

# What season C inherits from this season (contract §10, point 2 — named, not built)

- **The deployment shape as an index record.** After T3 the listener, the mode, the
  proxy and the certificate's expiry are facts Atlas holds nowhere: the index has four
  `PROPOSED` records and none about the gateway. A role document that reads "what is
  deployed" from Atlas needs someone to have proposed it as an `OPERATIONAL_FACT`. This
  plan adds no such step (the coordinator's instruction); it names the gap.
- **Executor role document, gateway tasks:** ask `atlas_code_impact` on
  `include/atlas/gwpolicy.h` before touching it — 14 direct dependents measured on
  2026-09-05 — and read `src/gw/gateway.c:530-547` before adding any policy key that
  names a host.
- **Policy keys C will add** sit in the same root-owned file whose parser refuses an
  unknown key; P0's rule applies — the binary that knows the key is deployed before the
  key is written — and the acceptance keys' refusal under `REVERSE_PROXY` is a
  precedent for "a key that has nothing to apply to is MALFORMED, not inert".
- **Verifier role document:** the grep-the-served-bytes discipline is the whole of what
  the suite establishes about the page; a Verifier that reports "the page remembers the
  key" from `test_gw_remote` is reporting that the bytes are present, not that a
  browser did it — T4's observations are the only evidence of the latter.

---

# Underspecified in the contract — questions, not choices

1. **"Come out of the policy"** — the lines only (this plan's reading: the parser keeps
   refusing them under `REVERSE_PROXY`, which is the guard that fires if the proxy is
   ever removed), or also the parser's support for the two keys?
2. **The season's name** for `ATLAS_PHASE`, the commit prefixes and the CLAUDE.md table:
   "A" as the contract calls it, or the next number after A16?
3. **Which devices** trust the certificate (row 1's second half) — the phone alone, or
   also a laptop browser on the LAN?
4. **`IPAddressDeny=any`** beside the gateway unit's existing `IPAddressAllow=localhost`,
   which is inert without it: once the listener is loopback, should the kernel enforce
   that? A hardening line not in the goal, so not in the tasks.
5. **The `clear:` / `clear-submit:` status lines** read "(not accepted -- … offered only
   behind tls_mode = REVERSE_PROXY)" when `REVERSE_PROXY` is the mode — accurate, but it
   reads as a refusal of the intended shape. Reword ("nothing to accept under
   REVERSE_PROXY") or leave? Not in the goal, so not in the tasks.
6. **§9 and the cut-over.** T3 is root edits and restarts — deploy, which §9 keeps with
   the Operator. Does the delegation cover anything of T3, or does the trial stop after
   T1 (and T2 once the rows are answered)? This plan reads it as stopping.
7. **§10's writing half.** Should the Planner record this plan's decisions as `PROPOSED`
   records now (`atlas_propose_decision`, one per decision), or does that wait for
   season C's role documents? None was written.
8. **The anonymous floor** (reason 569): built at the Operator's request for the restart
   problem on 2026-09-04 and never installed — is it part of "sign in once" (row 3 c)
   or superseded by the remembered login key?
9. **The second submission key's id** in the live policy (`key_1515cefd1f9f71a4`)
   differs from the one `docs/remote-submission.md` names (`key_01364e94e1dcbad4`).
   T5 dates the drift; whether a key was rotated and why is not in any document.
