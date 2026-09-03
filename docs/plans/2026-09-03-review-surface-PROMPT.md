You are writing the **season implementation plan** for Atlas' review surface — the season
that makes Mission Control the place a proposal is read, and decides how far it may go
towards disposing of one.

Write it to `/opt/atlas/docs/plans/2026-09-03-review-surface.md`. You write the plan. You do
not write production code and you do not dispatch subagents.

## Read first

1. `/opt/atlas/CLAUDE.md` — binding. Especially the **A4** rules (the approval contract, and
   the forbidden phrasings scanned by `tests/test_decision_mcp.c`), **A7/A7.1** (authority is
   configured outside the reach of the principal it constrains; a terminal is not authority),
   and **A9** (what the gateway cannot do is true because of who it runs as; one CSP header;
   no route becomes a socket message unless it matched the fixed table).
2. `/opt/atlas/docs/roadmap.md`, the section **"Later: the review surface, and where a
   proposal is disposed of"**. It is substantial and already grounded: it states the sentence
   the season exists for, what is true today, the three tiers, and six non-negotiables for
   the strongest tier. **Treat it as the brief, not as background.**
3. `/opt/atlas/docs/decision-lifecycle.md` — A4's model, the ledger, and the content-hash
   capability the local channel uses.
4. `/opt/atlas/docs/remote-access.md` — A9's gateway, credentials and Mission Control.
5. `/opt/atlas/docs/plans/2026-09-01-a12.1-reconciled-memory.md` — **for its shape only**.
   It pins interfaces, SQL, formats, refusal sentences and test obligations exactly, and
   leaves C function bodies to the executor. Match that.

## Verified for you, so you do not re-derive it

Checked against the tree today, 2026-09-03:

- **Mission Control is one file**: `src/gw/ui/mission-control.html`, 1009 lines. There is no
  build step and no framework.
- **The gateway serves 27 routes**, not the 26 the roadmap section records — it drifted since
  that section was written. **None of them mutates**; verify that yourself rather than taking
  it from me.
- **The listener carries both surfaces at once**: `remote_mcp = yes` and `web_gui = yes` on
  `192.168.0.198:8799`, `tls_mode = NONE`, `gateway_uid = 992`.
- **The external model is real and connected today.** API key `chatgpt-tunnel` was last used
  2026-09-03T17:58Z and holds five scopes, every one read-only:
  `context:read repo:read decisions:read graph:read impact:read`. The operator reaches Atlas
  as uid 1000 over the local socket; that key reaches it as a Bearer token over the gateway.
  **The two are already distinct identities** — this is the fact the season's threat argument
  turns on, and the roadmap section predates knowing it clearly.
- **A12.1 shipped** (79 commits, `24550bb..5a2c0ff`, plus a phase bump). Its drift finding
  exists, and the roadmap's "render A12.1's drift finding where there is one" needs
  tempering: A12.1's own whole-branch review established that **only one drift shape is
  actually producible** — a decision-bound SYMBOL anchor removed from a coverage-complete
  generation. A new claim is never evaluated; a re-mint evaluates the fresh claim, which
  passes by construction; a deleted path yields UNDETERMINED. `docs/context-reconciliation.md`
  now states this precisely. **A UI that implies broader drift detection would be advertising
  something the code does not do**, which is the defect A12.1 spent itself finding.

## The decision the plan must make, and make openly

The roadmap gives three tiers and deliberately does not choose. **Choosing is the plan's
first job**, with the argument written out — not a preference, a case. My reading, which you
should treat as one input and are free to reject with reasons:

Tier 1 (the UI reviews, the terminal disposes) buys most of the value and moves the threat
model by nothing. The expensive half of reviewing a proposal is *reading* it — every
revision, its evidence and counter-evidence, the gate results, the impact set — and that half
is pure rendering over routes that already exist. Tier 3's six non-negotiables are each real
work, and one of them (TLS) is not Atlas' to provide at all.

But the operator's stated purpose is that he approves from wherever he is, so a plan that
delivers tier 1 and calls the question closed would be answering a question he did not ask.
**Say what tier 1 leaves undone and what tier 3 would cost**, so the choice is his and is
informed, and make the plan's stages such that tier 1 ships standing on its own even if the
rest is never built.

## What must be true whatever you choose

Restate these in the plan, in full, with their reasons — they are not preferences:

- **No approval verb in an MCP tool name, ever.** `tests/test_decision_mcp.c` scans for
  `approve`, `approval`, `reject`, `supersede`, `confirm`, `sign`, `resolve`, `revalidate`,
  and the scanner is right.
- **Nothing may claim a channel establishes that a natural person acted.** The same test
  scans `CLAUDE.md` against a second list. `LOCAL_OPERATOR_CONFIRMED` identifies a channel,
  not a person, and a same-uid process driving a pseudo-terminal reaches it exactly as a
  person does. Every comparison in the plan is against that sentence and not a stronger one.
- **A mutating route needs its own channel identity in the audit row**, never
  `LOCAL_OPERATOR_CONFIRMED` reused — reusing it makes every audit row ever written
  retrospectively ambiguous, which is the one cost that cannot be paid back.
- **`memory:write` is the precedent**: in the vocabulary and not grantable. Any operator
  scope follows it.
- **The gateway has no filesystem read path** and Atlas terminates no TLS.

## Shape

Follow A12.1's plan exactly in shape: numbered decisions each with its argument; frozen
formats; a task list where each task names its files, the interfaces it produces, and its
test obligations; an acceptance table; and a stated worst-case cost. Pin every line reference
you cite **by reading it**, not by copying it from the roadmap — twelve of A12.1's plan
sentences turned out false during execution, and the plan is read again by every task.

Number the season by asking one question in the plan's header rather than assuming:
`docs/roadmap.md` currently calls **A14** (remote job submission) "Next", and the operator
has now put this season first. Say which number you are taking and note that the roadmap's
ordering needs the same edit.
