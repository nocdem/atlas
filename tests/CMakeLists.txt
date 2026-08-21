# Atlas - test suite.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# A small first-party harness plus CTest. No external test framework: the A0
# build must not depend on anything that is not already guaranteed present.

add_library(atlas_test_support STATIC
  support/harness.c
  support/fixture.c
  support/jsoncheck.c
)
target_include_directories(atlas_test_support PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src
)
# Forks a grandchild that outlives its parent unless the whole process group is
# signalled. The A8 process-tree termination test needs a real one.
add_executable(atlas-spawner tools/atlas_spawner.c)
target_compile_definitions(atlas-spawner PRIVATE _POSIX_C_SOURCE=200809L)
atlas_set_warnings(atlas-spawner)

# The marker helper stands in for a hostile git helper in the hardening tests.
add_executable(atlas-marker tools/atlas_marker.c)
target_compile_definitions(atlas-marker PRIVATE _POSIX_C_SOURCE=200809L)
atlas_set_warnings(atlas-marker)

# A11.5a-R2: a child that emits streamed progress records on a schedule, so the
# activity-based idle bound can be proved without a real model worker.
add_executable(atlas-streamer tools/atlas_streamer.c)
target_compile_definitions(atlas-streamer PRIVATE _POSIX_C_SOURCE=200809L)
atlas_set_warnings(atlas-streamer)

# A9.2.5. A parse child that fails transiently before it succeeds, so the
# per-unit retry can be tested end to end rather than recorded as untestable.
# It uses `atlas_sem_index_opts.atlas_exe`, which has been the child's path since
# A8-CI; nothing in production changes for it.
add_executable(atlas-flaky-parse tools/atlas_flaky_parse.c)
target_compile_definitions(atlas-flaky-parse PRIVATE
  _POSIX_C_SOURCE=200809L
  ATLAS_BIN="$<TARGET_FILE:atlas>"
)
add_dependencies(atlas-flaky-parse atlas)
atlas_set_warnings(atlas-flaky-parse)

target_compile_definitions(atlas_test_support PUBLIC
  _POSIX_C_SOURCE=200809L
  ATLAS_BIN="$<TARGET_FILE:atlas>"
  ATLAS_MARKER_BIN="$<TARGET_FILE:atlas-marker>"
  ATLAS_SPAWNER_BIN="$<TARGET_FILE:atlas-spawner>"
  ATLAS_FLAKY_PARSE_BIN="$<TARGET_FILE:atlas-flaky-parse>"
  ATLAS_STREAMER_BIN="$<TARGET_FILE:atlas-streamer>"
)
target_link_libraries(atlas_test_support PUBLIC atlas_core)
atlas_set_warnings(atlas_test_support)

# The standalone JSON checker: verification scripts must need no language runtime.
add_executable(atlas-jsoncheck tools/atlas_jsoncheck.c)
target_link_libraries(atlas-jsoncheck PRIVATE atlas_test_support)
atlas_set_warnings(atlas-jsoncheck)

# A3. The DNA-scale acceptance fixture generator. In C for the same reason the
# JSON checker is: a measurement fixture is part of the build, and the build
# depends on no language runtime. Deterministic from a fixed seed, because a
# performance number is only comparable when the input was identical.
add_executable(atlas-gen-ctree tools/atlas_gen_ctree.c)
atlas_set_warnings(atlas-gen-ctree)

# A4. The decision-scale acceptance fixture. It writes through the real
# `atlas_decision_apply` rather than through hand-written SQL, so it links the
# core library — a fixture built by a second write path would measure a database
# shape the real one never produces.
add_executable(atlas-gen-decisions tools/atlas_gen_decisions.c)
target_link_libraries(atlas-gen-decisions PRIVATE atlas_core)
target_compile_definitions(atlas-gen-decisions PRIVATE _POSIX_C_SOURCE=200809L)
atlas_set_warnings(atlas-gen-decisions)

set(ATLAS_TESTS
  # The fixture's own hygiene, asserted from outside a process that used it.
  test_fixture
  test_core
  test_safetext
  test_proc
  test_git_parse
  test_db
  test_datadir
  test_scan
  test_search
  test_diff
  test_worktree
  test_cli
  test_terminal
  test_git_hardening
  # --- A1 ---
  test_lock
  test_ipc
  test_migrate3
  test_unit
  test_reconcile
  test_daemon
  # --- A2 ---
  test_ai_schema
  test_ai_trust
  test_mcp
  test_mcp_roots
  test_ai_attribution
  test_hooks
  test_plugin
  # --- A3 ---
  test_code_lex
  test_code_graph
  test_sem
  test_sem_freshness
  test_sem_discovery
  test_sem_trust
  test_sem_auto
  test_daemon_responsive
  test_registry
  test_ops
  test_code_compdb
  test_code_trust
  test_code_analyzer
  # --- A4 ---
  test_decision_model
  test_decision_lifecycle
  test_migrate6
  test_decision_operator
  test_decision_mcp
  test_decision_bridge
  # --- A5 ---
  test_backup
  test_maintenance
  test_backup_live
  # --- A6: impact gates and stale-decision detection ---
  test_gate_model
  test_gate
  test_gate_operator
  test_gate_trust
  test_migrate7
  # --- A7: security review and trust-boundary hardening ---
  test_a7_authority
  test_a71_syspolicy
  # --- A8: the durable orchestration control plane ---
  test_orch_model
  test_orch_lifecycle
  test_orch_run
  test_orch_parallel
  test_a11_run
  test_a11_head_drift
  test_orch_transport
  test_orch_validation_wire
  test_orch_usage
  test_orch_memory
  test_orch_rpc
  test_orch_workspace
  test_orch_driver
  test_orch_snapshot
  # --- A12.0: the planned run ---
  test_plan_format
  test_plan_db
  test_plan_rpc
  test_plan_driver
  test_remote_equivalence
  test_operator_peer
  # The A8 operator-boundary fix: decision links over the socket, and the
  # local/socket agreement that keeps the two paths one behaviour.
  test_decision_link_rpc
  test_decision_edge
  test_migrate10
  # A8.2: a revision stores what it was given; a link change changes only links.
  test_decision_revision_integrity
  test_migrate9
  test_migrate8
  # --- A9: remote access, API credentials and the web control plane ---
  #
  # The credential format, the verifier and the scope vocabulary are pure
  # functions: bytes in, a digest or a verdict out. No database, no daemon, no
  # repository, so this belongs in the fast subset.
  test_hmac
  # The credential lifecycle drives the real binary against a fixture data
  # directory: created once, listed, revoked, rotated, and searched as raw
  # bytes for a plaintext that must not be there.
  test_apikey
  # The gateway policy matrix and the HTTP reader are pure functions: bytes in,
  # a verdict out. No socket, no daemon, no repository -- except the one case
  # that writes a policy file to prove a user-writable one is refused.
  test_gateway
  # Remote MCP end to end: HTTP bytes in, HTTP bytes out, against a live
  # fixture daemon with real credentials. No listening port is opened.
  test_gw_remote
  # --- A9.1: knowledge kinds, and a lifecycle state that closes a demand ---
  #
  # The vocabulary, the kind-aware transition table, the resolve capability,
  # both filters, migration 13 and the backup contract, driven against a real
  # database in process. No daemon and no repository, like the A4 lifecycle
  # suite, because what is under test is the model, the schema and the
  # migration rather than a transport.
  test_decision_kind
  test_verify_model
  test_verify_engine
  test_verify_absence
  test_verify_intake
  test_verify_product
)

foreach(test_name IN LISTS ATLAS_TESTS)
  add_executable(${test_name} ${test_name}.c)
  target_link_libraries(${test_name} PRIVATE atlas_test_support)
  atlas_set_warnings(${test_name})
  # The CLI tests execute the built binary, so make the dependency explicit.
  add_dependencies(${test_name} atlas atlas-jsoncheck atlas-marker)
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES TIMEOUT 300)
endforeach()

# A repository is required for the integration suites; a missing git makes them
# fail loudly rather than silently pass.
set_tests_properties(test_sem PROPERTIES LABELS "integration")
set_tests_properties(test_sem_freshness PROPERTIES LABELS "integration")
set_tests_properties(test_sem_discovery PROPERTIES LABELS "integration")
set_tests_properties(test_sem_trust PROPERTIES LABELS "integration")
# A live daemon and real MCP sessions, so it belongs to the serialised group.
set_tests_properties(test_registry PROPERTIES LABELS "daemon")
set_tests_properties(test_ops PROPERTIES LABELS "daemon")
set_tests_properties(test_scan test_search test_diff test_worktree test_cli test_terminal test_code_graph test_code_compdb
  test_code_analyzer test_git_hardening test_reconcile PROPERTIES LABELS "integration")
set_tests_properties(test_core test_safetext test_proc test_git_parse test_db test_datadir
  test_lock test_ipc test_migrate3 test_unit PROPERTIES LABELS "unit")
# The fixture suite forks children that run the binary, but needs no repository,
# no database and no daemon.
set_tests_properties(test_fixture PROPERTIES LABELS "unit")

# A2. test_ai_schema drives the storage layer in process; test_mcp and
# test_plugin fork the binary but need no daemon and no repository.
set_tests_properties(test_ai_schema test_mcp test_plugin PROPERTIES LABELS "unit")

# A3. The extractor is pure — bytes in, a bounded object out — so its suite needs
# no repository, no database and no daemon, and belongs in the fast subset.
set_tests_properties(test_code_lex PROPERTIES LABELS "unit")

# A9. The credential format and the scope vocabulary are pure functions.
set_tests_properties(test_hmac PROPERTIES LABELS "unit")
set_tests_properties(test_apikey PROPERTIES LABELS "integration")
set_tests_properties(test_gateway PROPERTIES LABELS "unit")
set_tests_properties(test_gw_remote PROPERTIES LABELS "daemon" TIMEOUT 900 RUN_SERIAL TRUE)

# A4. The content hash, the transition table and validation are pure functions —
# bytes in, a digest or a verdict out — so this suite needs no repository, no
# database and no daemon, and belongs in the fast subset.
set_tests_properties(test_decision_model PROPERTIES LABELS "unit")
# The lifecycle suite drives the state machine against a real database in
# process, like test_ai_schema: no daemon, no repository, no filesystem events.
set_tests_properties(test_decision_lifecycle test_migrate6 PROPERTIES LABELS "unit")
# A9.1. Same shape and the same reason: a real database in process, no daemon.
# It builds a schema-12 database from the migration list rather than winding a
# finished one back, so it genuinely exercises the four-table rebuild.
set_tests_properties(test_decision_kind PROPERTIES LABELS "unit")
# A9.2. The model tests need no database and are pure functions of counted
# inputs, which is what makes the scoring auditable; the engine tests build a
# real repository and drive migrations, so they are integration.
set_tests_properties(test_verify_model PROPERTIES LABELS "unit")
set_tests_properties(test_verify_engine PROPERTIES LABELS "integration")
set_tests_properties(test_verify_absence PROPERTIES LABELS "integration")
# Scans `verify_ops.h` for a field that could carry truth or coverage in from a
# caller: the absence-proof guarantee is an absent parameter, not a check.
target_compile_definitions(test_verify_absence PRIVATE ATLAS_SRC_DIR="${CMAKE_SOURCE_DIR}")
set_tests_properties(test_verify_intake PROPERTIES LABELS "integration")
# Drives a live daemon and the MCP adapter, so it is a daemon test and is
# serialised against the others for the inotify budget.
# O10 added a restart test and an idempotency test, each with its own full
# fixture, so the default 300 s no longer leaves headroom under a sanitiser —
# where the failure would surface as a ctest timeout and read as a hang.
set_tests_properties(test_verify_product PROPERTIES LABELS "daemon" TIMEOUT 900)
# The operator suite builds a real repository and drives the binary through a
# pseudo-terminal, so it is an integration test rather than a unit one.
set_tests_properties(test_decision_operator PROPERTIES LABELS "integration")
# The MCP surface is a property of the binary: no database, no daemon, no repository.
set_tests_properties(test_decision_mcp PROPERTIES LABELS "unit")
# test_ai_trust and test_hooks fork a live daemon and wait on filesystem events,
# so they carry the daemon label and its serialisation for the same reason
# test_daemon does: parallel daemons compete for the machine's inotify budget.
set_tests_properties(test_ai_trust test_hooks test_mcp_roots test_ai_attribution test_code_trust
  test_decision_bridge test_decision_link_rpc test_decision_edge
  test_decision_revision_integrity
  PROPERTIES LABELS "daemon" TIMEOUT 900 RUN_SERIAL TRUE)

# A5. Backup builds a real repository and drives the binary; maintenance drives
# the storage layer and the binary. Neither forks a daemon — the live-daemon
# cases (an online snapshot, and a restore refused while a daemon holds the
# lock) are in test_backup_live, which carries the daemon label and its
# serialisation.
set_tests_properties(test_backup test_maintenance PROPERTIES LABELS "integration")
set_tests_properties(test_backup_live PROPERTIES LABELS "daemon" TIMEOUT 900 RUN_SERIAL TRUE)

# A6. The vocabularies, the fold and the digests are pure functions — inputs in,
# a verdict or a digest out — so this suite needs no repository, no database and
# no daemon, and belongs in the fast subset.
set_tests_properties(test_gate_model PROPERTIES LABELS "unit")
# The gate suite builds real repositories, indexes them through the CLI and
# assesses them in process, so it is an integration test.
set_tests_properties(test_gate PROPERTIES LABELS "integration")
# The revalidation suite drives the binary through a pseudo-terminal, like A4's
# operator suite and for the same reason: the requirement is that a terminal is
# needed, and a stubbed one would be testing the stub.
set_tests_properties(test_gate_operator PROPERTIES LABELS "integration")
# The trust suite forks a live daemon to ask it for method names that must not
# exist, so it carries the daemon label and its serialisation.
set_tests_properties(test_gate_trust PROPERTIES LABELS "daemon" TIMEOUT 900 RUN_SERIAL TRUE)
# It also reads Atlas' own documentation and source for the claim tripwire.
target_compile_definitions(test_gate_trust PRIVATE ATLAS_SRC_DIR="${CMAKE_SOURCE_DIR}")
# A7 forks a live daemon to ask it for the authority methods that must not
# exist, and drives the CLI through a pseudo-terminal to prove a locked profile
# is not opened by one. Daemon label, and serialised with the other daemon tests.
set_tests_properties(test_a7_authority PROPERTIES LABELS "daemon" TIMEOUT 900 RUN_SERIAL TRUE)
# A7.1 drives a live daemon to prove a client cannot describe its own identity,
# and checks the system-policy decision procedure against real filesystem shapes.
set_tests_properties(test_a71_syspolicy PROPERTIES LABELS "daemon" TIMEOUT 900 RUN_SERIAL TRUE)

# A8. The vocabularies, the transition table, the canonical digest and the
# policy loader are pure functions and real filesystem shapes, so this suite
# needs no repository, no database and no daemon: it belongs in the fast subset.
set_tests_properties(test_orch_model PROPERTIES LABELS "unit")
set_tests_properties(test_orch_validation_wire PROPERTIES LABELS "unit")
set_tests_properties(test_orch_usage PROPERTIES LABELS "unit")
# A12.0. The `atlas-plan-1` parser and the five prompt composers are pure
# functions — untrusted bytes in, a document or a refusal out — so this suite
# needs no repository, no database and no daemon either.
set_tests_properties(test_plan_format PROPERTIES LABELS "unit")
# A12.0. Migration 25, the plan's one write point and the derived status. It
# builds a real fixture repository, registers it through the CLI and drives both
# write points against an isolated database, so it is an integration test. It
# creates no worker, opens no socket and sleeps for nothing. It also scans
# `src/` for a second writer of the plan tables, which is what
# `ATLAS_SRC_DIR` is for.
set_tests_properties(test_plan_db PROPERTIES LABELS "integration" TIMEOUT 120)
target_compile_definitions(test_plan_db PRIVATE ATLAS_SRC_DIR="${CMAKE_SOURCE_DIR}")
# A12.0. The four `plan.` methods, at the edge. One case forks a live fixture
# daemon to ask what the protocol offers and what it must not, so the suite
# carries the daemon label and its serialisation for the reason every other
# daemon suite does: parallel daemons compete for the machine's inotify budget.
# The remaining cases drive `atlas_server_dispatch` in process against a real
# writer thread, which needs no socket and no second daemon.
set_tests_properties(test_plan_rpc PROPERTIES LABELS "daemon" TIMEOUT 900 RUN_SERIAL TRUE)
# A12.0's plan driver, hosted on a transport faked entirely in memory: no
# database, no repository, no socket and no process. It proves the loop's own
# refusals and one iteration of it; the full lifecycle against real runs is
# T8's. Labelled integration because it links the whole service layer, not
# because it touches anything outside its own process.
set_tests_properties(test_plan_driver PROPERTIES LABELS "integration")
# The lifecycle suite builds a real fixture repository, registers it through the
# CLI and drives the one write point against an isolated database, so it is an
# integration test. It sleeps for nothing: expiry and deadlines are supplied.
set_tests_properties(test_orch_lifecycle PROPERTIES LABELS "integration")
set_tests_properties(test_orch_run PROPERTIES LABELS "integration")
# A11.6's bounded parallelism, against a real fixture database. It builds a
# repository, registers it through the CLI and drives the one write point, so it
# is an integration test. It creates no worker, opens no socket and sleeps for
# nothing: every interleaving is issued in an order the test chose and every
# expiry is supplied to the operation.
set_tests_properties(test_orch_parallel PROPERTIES LABELS "integration" TIMEOUT 120)
set_tests_properties(test_orch_memory PROPERTIES LABELS "integration")
target_compile_definitions(test_orch_memory PRIVATE ATLAS_SRC_DIR="${CMAKE_SOURCE_DIR}")
set_tests_properties(test_a11_run PROPERTIES LABELS "integration")
# The step 6 suite builds a real fixture repository with a second real commit,
# drives one worker that moves HEAD onto it, and proves the gates never ran.
set_tests_properties(test_a11_head_drift PROPERTIES LABELS "integration")
# A12.0's transport recovery. Real repository, real worker, real settlement, and
# a fixture transport that loses selected answers on purpose. It waits a
# millisecond between attempts rather than the compiled two seconds, so the
# recovery is what is measured and not the pause.
set_tests_properties(test_orch_transport PROPERTIES LABELS "integration")
# The RPC suite drives a live daemon to prove the negative enumeration still
# holds and that a dispatcher method is unreachable from a client connection.
set_tests_properties(test_orch_rpc PROPERTIES LABELS "daemon" TIMEOUT 900 RUN_SERIAL TRUE)
# Migration 7 -> 8 and its rollback, against real databases built in a fixture.
set_tests_properties(test_migrate8 PROPERTIES LABELS "integration")
# The workspace suite builds real repositories, snapshots them and harvests the
# result, so it is an integration test. It touches nothing outside its fixture.
set_tests_properties(test_orch_workspace PROPERTIES LABELS "integration")
# The driver suite runs real child processes to prove the execution boundary:
# structured argv, a built environment, idle bounds and process-group kill.
set_tests_properties(test_orch_driver PROPERTIES LABELS "integration" TIMEOUT 300)
# The snapshot suite drives the daemon-side read against a fixture repository
# and a fixture database: the worker never appears in it, which is the point.
set_tests_properties(test_orch_snapshot PROPERTIES LABELS "integration" TIMEOUT 300)
set_tests_properties(test_remote_equivalence PROPERTIES LABELS "daemon" TIMEOUT 300)
set_tests_properties(test_operator_peer PROPERTIES LABELS "unit" TIMEOUT 60)
set_tests_properties(test_migrate9 PROPERTIES LABELS "integration" TIMEOUT 120)
set_tests_properties(test_migrate10 PROPERTIES LABELS "integration" TIMEOUT 120)
target_compile_definitions(test_a7_authority PRIVATE ATLAS_SRC_DIR="${CMAKE_SOURCE_DIR}")
# Migration 7 rebuilds a table, so its suite drives the storage layer and the
# backup CLI against real databases. No daemon and no repository.
set_tests_properties(test_migrate7 PROPERTIES LABELS "integration")

# The plugin fixture test reads the shipped plugin from the source tree, so it
# has to be told where that is rather than guessing from the working directory.
# The claim tripwire reads Atlas' own documentation and source, so it has to be
# told where the source tree is rather than guessing from the working directory.
target_compile_definitions(test_decision_mcp PRIVATE ATLAS_SRC_DIR="${CMAKE_SOURCE_DIR}")
# A5 has its own claim tripwire, over the operational documentation and the
# backup and maintenance source, for the same reason and by the same device.
target_compile_definitions(test_backup PRIVATE ATLAS_SRC_DIR="${CMAKE_SOURCE_DIR}")

target_compile_definitions(test_plugin PRIVATE
  ATLAS_PLUGIN_DIR="${CMAKE_SOURCE_DIR}/integrations/claude/atlas"
  ATLAS_MARKETPLACE_DIR="${CMAKE_SOURCE_DIR}/integrations/claude")

# The daemon suite forks the real binary and waits on filesystem events, so it is
# both slower and more timing-sensitive than the rest. It is labelled separately
# so `ctest -L daemon` and `ctest -LE daemon` are both useful.
set_tests_properties(test_daemon PROPERTIES LABELS "daemon" TIMEOUT 900)
# It also serialises against itself: several daemons watching several fixtures at
# once would compete for the machine's inotify watch budget, which would make the
# watch-limit degradation path fire for reasons that have nothing to do with the
# code under test.
set_tests_properties(test_daemon PROPERTIES RUN_SERIAL TRUE)

# A9.2.3's automatic-rebuild suite is a daemon suite for the same reasons, and
# additionally runs a compiler: its waits are for observable outcomes with a
# deadline rather than for a guessed interval, so a slow machine takes longer
# instead of failing. Serialised for the inotify reason above and because two
# fixtures compiling at once is not what any of these tests is measuring.
set_tests_properties(test_sem_auto PROPERTIES LABELS "daemon" TIMEOUT 1200)
# Serialised against every other daemon test: this one asserts how long the
# daemon takes to answer, and a second daemon competing for the machine would
# make a real regression gate flaky for a reason that is not the daemon.
# O10 added a third test to it, and each one drives a full semantic pass to
# CURRENT over the 160-unit fixture, so the budget is three passes rather than
# two. Raised rather than left to be discovered as a timeout under a sanitiser.
set_tests_properties(test_daemon_responsive
  PROPERTIES LABELS "daemon" TIMEOUT 2100 RUN_SERIAL TRUE)
set_tests_properties(test_sem_auto PROPERTIES RUN_SERIAL TRUE)

# Vendored third-party source is only trustworthy if it is still what was
# vendored, so the digest check is part of the suite rather than a manual step.
# /bin/sh, not bash, and no language runtime.
add_test(NAME vendor_digests
  COMMAND /bin/sh ${CMAKE_SOURCE_DIR}/scripts/verify_third_party.sh)
set_tests_properties(vendor_digests PROPERTIES LABELS "unit")
