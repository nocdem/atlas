# Atlas intelligence improvements — 2026-09-18

The user requested the five review recommendations in sequence. Preserve the
existing worktree; its source snapshot and diff are under
`/tmp/atlas-intelligence-baseline-20260918/`. Validation uses an isolated build
and fixture repositories, never the installed service or its database.

1. Improve task seeding: preserve paths and identifier case, bounded partial
   symbol lookup, explicit MCP seeds and CLI/daemon parity. Report ambiguity
   and candidate limits rather than silently choosing one symbol.
2. Improve context selection: prefer relevant repository code over incidental
   external calls; retain representation of related tests and knowledge within
   the existing budget. Keep evidence strength separate from relevance.
3. Select earlier runs using their recorded gates, failure details and changed
   paths as well as their goals. Explain which signals matched; preserve frozen
   packages and proposal-only authority.
4. Attach bounded, actionable read suggestions to context gaps, with scope and
   coverage limitations stated explicitly.
5. Improve test recommendations with declared test roots, transitive references
   and available test-target/evaluation evidence. State the basis and gaps; no
   suggested test set claims exhaustive coverage.

Each step requires focused regressions through its public surfaces. Retrieval
quality checks accompany the existing size/timing benchmark. Actual results,
limitations and implementation details will be recorded below as work proceeds.

## Progress

- Step 1: implemented. Semantic regressions, the real CLI retrieval benchmark,
  every-tool MCP argument checks and CLI/MCP/daemon parity passed. The daemon
  test first failed to bind under the sandbox and passed outside it using
  private fixture paths. Turkish prose can be paired with explicit technical
  scope; this change does not translate natural-language concepts.
- Step 2: implemented. A three-item regression retains the subject, a related
  test and a proposal requested with history, preserving its PROPOSED/LEXICAL
  labels. Semantic tests and the CLI retrieval benchmark passed.
- Step 3: implemented; the database-to-frozen-package regression passed. Matching
  now uses recorded gates/output and bounded paths from the last retained patch.
  These are historical hints, not proof of deployment or of a failing result.
- Step 4: implemented. CLI, MCP and daemon responses carry bounded file scope
  diagnostics and focused read suggestions. Covered and uncovered fixture files
  retain their correct presence/UNKNOWN distinctions.
- Step 5: implemented. Declared test roots, transitive call paths and stored TEST
  evidence provide explained recommendations. Regressions check historical
  results, latest-record selection, unindexed paths and CLI/MCP/daemon parity.
  Target mappings require stored evidence; CMake targets are not guessed.

## Validation

- Focused Release suite: `test_sem`, `test_sem_trust`,
  `test_context_benchmark`, `test_orch_memory`, `test_mcp_arguments`: 5/5 passed.
- Full Release CTest suite: 132/132 passed (426.56 seconds), including vendor
  digest checks, CLI, MCP, daemon, gateway and orchestration fixtures.
- AddressSanitizer, LeakSanitizer and UndefinedBehaviorSanitizer:
  `test_sem`, `test_context_benchmark`, `test_orch_memory`, `test_mcp_arguments`
  passed, 4/4. The initial sandbox run could not run LeakSanitizer under ptrace;
  the same checks passed outside the sandbox. No real service data was used.
- `ATLAS_WITH_LIBCLANG=OFF`, Release with warnings as errors: build passed;
  `test_sem` and `test_mcp_arguments` passed. The compiler-dependent benchmark
  explicitly skipped with exit 77, rather than reporting successful recall.
- Final review compared against the pre-edit source snapshot; `git diff --check`
  passed. The installed daemon was not replaced or restarted.
