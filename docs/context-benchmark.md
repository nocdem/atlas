# Context measurements

Run `make benchmark-context JOBS=4`, or run
`BUILD_DIR/tests/test_context_benchmark` after building that target. The program
writes CSV to stdout and assertions to stderr. CTest also runs it under the
`benchmark` label. It requires libclang; without it the test explicitly reports
that measurements were skipped (exit 77, displayed as Skipped by CTest).
No model, account, live daemon or registered
repository is used.

## What the fixture measures

The fixture creates three C translation units, one header, one isolated function,
one shared function and 80 callers. It commits and indexes a temporary repository
through the real CLI. Each task gets one warmup per profile and eleven timed
samples, alternating profile order. Indexing is excluded; process startup,
database reads, freshness checks and JSON rendering are included. The clock is
monotonic; p50 is the sixth sorted sample and p95 the eleventh. There is no
machine-dependent latency pass threshold.

The previous profile explicitly requests 400 items and 8,192 approximate tokens
(32 KiB); the compact profile uses the actual defaults of 24 items and 8 KiB.
This compares retrieval profiles in the current implementation, not two historical
binaries. Both profiles now disclose item-limit omissions. Required symbol
presence, selection provenance, byte/item bounds and gap disclosure are asserted
on every response. Broad fixtures must contain at least 80 items in the previous
profile and return smaller JSON with the compact profile.

`used_bytes` counts the context builder's item budget. `wire_bytes` counts the
complete CLI JSON, including metadata. Neither is a measured model token count.
MCP envelopes add further bytes. Reducing included items reduces output; it does
not avoid collecting and ranking the candidate set first.

## Observed on 2026-09-14

Linux x86_64, GCC 12.2, SQLite 3.40.1, libclang 14, Release, warnings as errors.
These are local synthetic measurements; they do not establish model task success
or production throughput. Raw observations are in
`benchmarks/2026-09-14-context-profiles.csv`.

| Task | Items, previous → compact | JSON bytes, previous → compact | p50 ms, previous → compact |
| --- | --- | --- | --- |
| Isolated symbol `alpha_unique` | 1 → 1 | 1,272 → 1,271 | 26.42 → 26.16 |
| Cross-file `shared_core` | 81 → 24 | 12,141 → 4,447 | 28.03 → 27.94 |
| Handoff naming three symbols | 82 → 24 | 12,293 → 4,463 | 28.59 → 28.54 |
| Prefix-only `work_worker` | 0 → 0 | 1,250 → 1,249 | 25.11 → 25.33 |
| Nonexistent symbol | 0 → 0 | 1,245 → 1,244 | 25.49 → 25.52 |

The two broad queries emit about 63% fewer JSON bytes and disclose the omitted
items. Their named seed symbols remain present. This does **not** prove that the
24-item package contains every caller needed for an exhaustive change; use a
targeted graph query or increase the limit for that task. Latency is essentially
unchanged in this run, and the isolated-symbol compact p95 was worse (36.38 ms
versus 27.87 ms). There is no demonstrated speed win.

The historical prefix-only row is a retrieval limitation, not successful recall
of those 80 functions. The initial benchmark incorrectly expected prefix recall
and failed. Since 2026-09-18 the implementation supports bounded lexical prefix
lookup and the fixture requires prefix recall, including the candidate selection
reason and any seed-limit disclosure. A file-path retrieval case was added too.
The historical CSV above is unchanged; it does not measure the new behavior.

## End-to-end Atlas on/off protocol

Retrieval measurements cannot establish whether an agent finishes sooner or uses
fewer billed tokens. For that experiment use isolated workspaces at the same
commit and a frozen task/evaluation set. Cover small known-file edits, cross-file
changes, debugging, history/decision questions and handoffs. Use at least three
pairs per category, alternating on/off order. Keep the model version, reasoning
setting, prompts, allowed tools, gate commands, worker count and timeout fixed.
Both arms receive identical repository instructions; the on arm alone receives
the specified Atlas context/tools. Separate cold indexing cost from warm queries.
Record the exact memory mode: orchestration memory packs and MCP code context
are separate mechanisms and should be separate experiments.

For every run retain task ID, commit, profile, model/settings, start/end times,
correctness gate result, required facts/citations, tool calls, raw provider usage
(uncached input, cached input, output and reasoning when available), actual price
schedule, retry count and human interventions. Missing usage is `unmeasured`,
never zero. Include failed and timed-out runs. Gate correctness before comparing
cost; report paired deltas and their spread, not only the best run or pooled mean.
Do not pool categories with different failure rates into a single benefit claim.

No new paid model experiment was run for this change. The historical observations
in `plans/2026-09-08-memory-offon-benchmark-minimal.md` remain historical evidence;
the CSV above neither replaces them nor establishes an end-to-end improvement.
