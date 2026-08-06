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
# The marker helper stands in for a hostile git helper in the hardening tests.
add_executable(atlas-marker tools/atlas_marker.c)
target_compile_definitions(atlas-marker PRIVATE _POSIX_C_SOURCE=200809L)
atlas_set_warnings(atlas-marker)

target_compile_definitions(atlas_test_support PUBLIC
  _POSIX_C_SOURCE=200809L
  ATLAS_BIN="$<TARGET_FILE:atlas>"
  ATLAS_MARKER_BIN="$<TARGET_FILE:atlas-marker>"
)
target_link_libraries(atlas_test_support PUBLIC atlas_core)
atlas_set_warnings(atlas_test_support)

# The standalone JSON checker: verification scripts must need no language runtime.
add_executable(atlas-jsoncheck tools/atlas_jsoncheck.c)
target_link_libraries(atlas-jsoncheck PRIVATE atlas_test_support)
atlas_set_warnings(atlas-jsoncheck)

set(ATLAS_TESTS
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
set_tests_properties(test_scan test_search test_diff test_worktree test_cli test_terminal
  test_git_hardening test_reconcile PROPERTIES LABELS "integration")
set_tests_properties(test_core test_safetext test_proc test_git_parse test_db test_datadir
  test_lock test_ipc test_migrate3 test_unit PROPERTIES LABELS "unit")

# The daemon suite forks the real binary and waits on filesystem events, so it is
# both slower and more timing-sensitive than the rest. It is labelled separately
# so `ctest -L daemon` and `ctest -LE daemon` are both useful.
set_tests_properties(test_daemon PROPERTIES LABELS "daemon" TIMEOUT 900)
# It also serialises against itself: several daemons watching several fixtures at
# once would compete for the machine's inotify watch budget, which would make the
# watch-limit degradation path fire for reasons that have nothing to do with the
# code under test.
set_tests_properties(test_daemon PROPERTIES RUN_SERIAL TRUE)

# Vendored third-party source is only trustworthy if it is still what was
# vendored, so the digest check is part of the suite rather than a manual step.
# /bin/sh, not bash, and no language runtime.
add_test(NAME vendor_digests
  COMMAND /bin/sh ${CMAKE_SOURCE_DIR}/scripts/verify_third_party.sh)
set_tests_properties(vendor_digests PROPERTIES LABELS "unit")
