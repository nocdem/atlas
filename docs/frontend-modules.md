# CLI and MCP modules

The September 2026 split changes private linkage and source locations while
preserving public commands, tool names, scopes, schemas and service calls.
Production sources remain explicitly listed in CMake.

| Responsibility | CLI | MCP |
| --- | --- | --- |
| Entry and dispatch | `cli.c`, `cli_dispatch.c` | `mcp_tools.c` |
| Help and argument parsing | `cli_args.c` | Each tool's schema/handler |
| Shared adapter helpers | `cli_common.c` | `mcp_tools_common.c` |
| Daemon, integration and routing | `cli_daemon.c`, `cli_remote.c` | `mcp.c` |
| Repository reads | `cli_dispatch.c` | `mcp_tools_read.c` |
| Code and task context | `cli_code.c` | `mcp_tools_code.c`, `mcp_tools_sem.c` |
| Decisions, review, gate and verification | `cli_decision.c` | `mcp_tools_memory.c`, `mcp_tools_gate.c`, `mcp_tools_verify.c` |
| Canonical memory | `cli_memory.c` | Existing decision/verification surfaces |
| Jobs and plans / remote jobs and deploy | `cli_jobs.c` | `mcp_tools_jobs.c`, `mcp_tools_deploy.c` |

Paths are relative to `src/cli` or `src/mcp`. Private cross-module declarations
live in `cli_internal.h` and `mcp_tools_internal.h`; feature-local callbacks stay
static. Keep services and persistence outside these adapters. Approval refusal,
credential scopes and repository-root checks retain their existing enforcement
points. The MCP registry validates keys against the published schema before a
feature handler validates values and required fields.

For a new CLI form update help/parsing, dispatch, its handler and the command/remote
allowlists. For a new MCP tool update its feature module, private declarations,
registry and every-tool contract tests. Historical phase documents may still
name the former monolithic files; this table is the current map.

## Automated checks

`.github/workflows/ci.yml` runs the full Release CTest suite and CLI smoke checks
on pushes and pull requests. Separate jobs exercise unit and selected CLI/semantic
checks with ASan+UBSan. The build without libclang runs unit/CLI checks and the
explicit unavailable-compiler contract in `test_sem`. TSan runs lock, writer/watcher
and daemon suites separately from ASan. Failures are not ignored; test logs are
retained for 14 days. Skipped no-libclang measurements are not successful semantic
benchmarks.

Run daemon/watcher tests from one CTest process at a time on a shared machine.
`RUN_SERIAL` serializes tests inside one invocation, not across build directories.
The local review encountered `kernel_limit` with overlapping watcher suites; the
same Release test passed in isolation and the TSan suite passed separately.

Reproduce the alternate configurations with `-DATLAS_ASAN=ON -DATLAS_UBSAN=ON`,
`-DATLAS_TSAN=ON`, or `-DATLAS_WITH_LIBCLANG=OFF` in separate CMake build
directories. `ATLAS_WERROR` remains enabled. The libclang switch overrides a
previously cached discovery result when reconfiguring the same directory.

The workflow follows the [GitHub Actions syntax](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax)
and pins [checkout v7.0.1](https://github.com/actions/checkout/releases/tag/v7.0.1)
and [upload-artifact v7.0.1](https://github.com/actions/upload-artifact/releases/tag/v7.0.1)
to release commits. It needs read access to repository contents and persists no
checkout credential. Hosted execution requires the workflow to reach GitHub;
local validation alone does not establish that a hosted run passed.
