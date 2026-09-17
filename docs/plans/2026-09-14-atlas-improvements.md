# Atlas improvements — 2026-09-14

The user asked to implement the five recommendations from the repository review
in order. Existing uncommitted work is preserved. A pre-change source snapshot
and the user's diff are saved outside the repository in `/tmp` for comparison.

1. **Correctness:** reject NUL-bearing claim text before persistence; enforce
   published MCP argument keys at the shared dispatcher, including remote-only
   tools. Add regression coverage and close the corresponding backlog entries.
2. **Current documentation:** align the product, architecture and data-model
   introductions with the implemented capabilities and schema; clearly separate
   rebuildable indexes from canonical records that need backups.
3. **Economical context:** make bounded task context the normal entry point;
   improve compact defaults and disclosure of omitted results; move detailed
   agent reference material out of the always-loaded instructions.
4. **Measurement:** provide reproducible task categories and measurements with
   explicit correctness expectations, timing and context-size accounting.
   Distinguish retrieval measurements from end-to-end model outcomes; do not
   invent token usage or human-intervention measurements.
5. **Maintainability:** split CLI/MCP implementation into feature modules while
   preserving the public surfaces, and add automated build, test and sanitizer
   checks.

Validation uses `/tmp/atlas-review-20260914` so the running installation and its
existing compilation database are preserved. The review baseline was a clean
Release build, 130 passing CTest entries and 62 passing CLI smoke checks.

## Progress

- Step 1: complete. NUL intake, every-tool argument checks and eight relevant
  CTest entries passed, including live verification and remote submission.
  Existing transport tests now expect an invalid-parameters response when a
  caller supplies an undeclared field, and retain the valid-call checks.
- Step 2: complete. Current introductory docs distinguish indexes and canonical
  records, describe optional editing/deployment and report schema 33. A freshly
  built `atlas --json version` reports phase A17 and schema 33.
- Step 3: complete. Compact context defaults, item-limit disclosure and shorter
  agent entry points passed six relevant CTest entries. Detailed prior
  instructions, including the user's existing edits, remain in `docs/agent-reference.md`.
- Step 4: complete for local retrieval measurement. Five fixture categories,
  alternating profiles and eleven samples per profile pass correctness and
  disclosure checks. Broad JSON responses shrank about 63%; latency did not
  materially improve. Prefix-only retrieval returned no symbols. Results and the
  separate, not-yet-run end-to-end model protocol are in `docs/context-benchmark.md`.
- Step 5: complete. Feature split and CI implemented. CLI entry is 199 lines and MCP
  registry/dispatch 769 lines; the largest extracted module is 1,216 lines.
  A comparison preserved all 65 CLI and 156 MCP function bodies apart from private
  symbol renames. Authority source checks follow the moved files.
  The CI matrix defines Release, no-libclang, ASan+UBSan and TSan jobs.
  Debug builds exposed two existing format-truncation warnings: the remote
  failure explanation now has sufficient space, and deploy result filenames
  have an explicit bound before copying.

## Validation record

- Release: all targets build with warnings as errors. The full 132-entry CTest
  run passed 131 entries; `test_watch_budget` reported `kernel_limit` while
  watcher suites from separate build directories overlapped. The unchanged
  Release watcher suite passed when run alone (24 internal cases, 21.69 s).
  This is recorded as a failed run followed by a successful isolated rerun,
  not as an uninterrupted green full run.
  After the final source changes, all five targeted entries (MCP arguments,
  semantics, context benchmark, remote submission and deploy RPC) passed.
- CLI smoke: 62 checks passed after the final source changes.
- ASan/LSan + UBSan: all 44 unit entries passed, followed by all four CLI,
  semantic/trust and context benchmark entries. The NUL intake suite also
  passed under these sanitizers in the earlier five-entry targeted run.
- TSan: lock, daemon and watcher suites all passed in a separate invocation
  (134.74 s). Sandbox-restricted attempts could not run the daemon or leak
  checker correctly; the successful results are from unrestricted executions.
- Without libclang: all 44 unit entries, the CLI suite and the dedicated
  unavailable-compiler contract passed. Context measurements explicitly skip
  with exit 77. Compiler-required semantic/trust cases are not reported as
  passing in this profile.
- Vendored digests, skill validation, workflow YAML and new documentation links
  pass. CLI/MCP function-body comparison and review against the pre-existing
  worktree snapshot preserve the user's changes.

The GitHub workflow is ready but has not been run on GitHub. Builds and fixtures
stay under `/tmp`; the installed Atlas service was not upgraded. The local
retrieval benchmark does not establish an end-to-end model token/cost benefit.
