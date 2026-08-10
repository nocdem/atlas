# Atlas - generic, headless engineering-memory and repository-intelligence CLI.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.

cmake_minimum_required(VERSION 3.16)

project(Atlas
  VERSION 0.1.0
  DESCRIPTION "Atlas engineering memory and repository intelligence"
  LANGUAGES C
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ATLAS_ASAN "Build with AddressSanitizer and LeakSanitizer" OFF)
option(ATLAS_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
option(ATLAS_TSAN "Build with ThreadSanitizer" OFF)
option(ATLAS_WERROR "Treat warnings in first-party code as errors" ON)
option(ATLAS_BUILD_TESTS "Build the test suite" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(cmake/AtlasWarnings.cmake)

# --- dependencies -----------------------------------------------------------
# No FetchContent, no downloads: every dependency must already be present on the
# system, and a missing one is a hard, clearly reported failure.

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
message(STATUS "Atlas: SQLite3 ${SQLITE3_VERSION} via pkg-config")

find_program(GIT_EXECUTABLE git)
if(NOT GIT_EXECUTABLE)
  message(WARNING
    "Atlas: git was not found at configure time. Atlas resolves git from PATH at "
    "runtime, so this is only a warning, but the test suite requires git.")
else()
  message(STATUS "Atlas: git found at ${GIT_EXECUTABLE}")
endif()

# --- sanitizers -------------------------------------------------------------

set(ATLAS_SANITIZE_FLAGS "")
if(ATLAS_ASAN)
  list(APPEND ATLAS_SANITIZE_FLAGS -fsanitize=address -fno-omit-frame-pointer)
endif()
if(ATLAS_UBSAN)
  list(APPEND ATLAS_SANITIZE_FLAGS
    -fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
endif()
if(ATLAS_TSAN)
  # ThreadSanitizer is mutually exclusive with ASan, so the build types are
  # separate directories rather than composable flags.
  if(ATLAS_ASAN)
    message(FATAL_ERROR "Atlas: ATLAS_TSAN and ATLAS_ASAN cannot be combined")
  endif()
  list(APPEND ATLAS_SANITIZE_FLAGS -fsanitize=thread -fno-omit-frame-pointer -g)
endif()

# --- vendored third-party ---------------------------------------------------
# yyjson is vendored, not downloaded: see third_party/yyjson/PROVENANCE.md for the
# exact upstream tag, archive digest and per-file digests, and
# scripts/verify_third_party.sh for the check that they still hold.
#
# ATLAS_WERROR is deliberately NOT applied here. Warnings are errors in
# first-party code; making them errors in upstream code would force edits to
# upstream source, which would break the byte-for-byte guarantee that is the
# whole point of pinning it.

add_library(atlas_yyjson STATIC third_party/yyjson/yyjson.c)
target_include_directories(atlas_yyjson PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/yyjson)
target_compile_definitions(atlas_yyjson PRIVATE
  # Atlas only ever reads JSON with yyjson; the writer half is first-party.
  YYJSON_DISABLE_WRITER=1
  # No incremental/streaming reader is used, and no float formatting is needed.
  YYJSON_DISABLE_UTILS=1
)
set_target_properties(atlas_yyjson PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(ATLAS_SANITIZE_FLAGS)
  target_compile_options(atlas_yyjson PUBLIC ${ATLAS_SANITIZE_FLAGS})
  target_link_options(atlas_yyjson PUBLIC ${ATLAS_SANITIZE_FLAGS})
endif()

# --- threads ----------------------------------------------------------------
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# --- core library -----------------------------------------------------------

add_library(atlas_core STATIC
  src/core/error.c
  src/core/buf.c
  src/core/sha256.c
  src/core/util.c
  src/core/pathrep.c
  src/core/proc.c
  src/core/datadir.c
  src/core/diff.c
  src/core/safetext.c
  src/core/scan.c
  src/core/service.c
  src/db/db.c
  src/db/migrate.c
  src/db/db_repo.c
  src/db/db_index.c
  src/db/db_search.c
  src/db/db_state.c
  src/db/db_code.c
  src/git/git.c
  src/git/git_harden.c
  src/git/git_parse.c
  src/output/json.c
  src/cli/cli.c
  src/cli/render_human.c
  src/cli/render_json.c
  # --- A1: locking, IPC, daemon, watcher --------------------------------
  src/core/lock.c
  src/core/unit.c
  src/core/reconcile.c
  src/ipc/frame.c
  src/ipc/proto.c
  src/ipc/sock.c
  src/ipc/client.c
  src/ipc/server.c
  src/daemon/writer.c
  src/daemon/workers.c
  src/daemon/watch.c
  src/daemon/daemon.c
  src/core/service_daemon.c
  # --- A2: AI session model, MCP adapter, hook adapter, integration ------
  src/db/db_ai.c
  src/ai/ai.c
  src/ai/context.c
  src/ipc/reply.c
  src/ipc/json_read.c
  src/ipc/server_ai.c
  src/ipc/server_code.c
  src/hook/hook.c
  src/mcp/mcp.c
  src/mcp/mcp_tools.c
  src/core/integrate.c
  src/core/service_code.c
  # --- A3: structural code intelligence ---------------------------------
  src/code/code.c
  src/code/extract.c
  src/code/compdb.c
  src/code/resolve.c
  src/code/index.c
  src/code/query.c
  # --- A4: decision documents, revisions and operator approval -----------
  src/db/db_decision.c
  src/decision/decision.c
  src/decision/lifecycle.c
  src/core/terminal.c
  src/core/service_decision.c
  src/ipc/server_decision.c
  # --- A5: operational backup, verification, restore and maintenance -----
  #
  # There is deliberately no src/ipc file here. Backup, restore and
  # maintenance are local CLI operations with no RPC method, so nothing
  # reachable from MCP or a hook can replace or prune the index.
  src/db/db_backup.c
  src/core/service_backup.c
  src/db/db_maintenance.c
  src/core/service_maintenance.c
  # --- A6: impact gates and stale-decision detection ---------------------
  #
  # The gate reads. It creates no process, writes no row and takes no lock,
  # which is what makes "normal read-only indexing is never blocked by the
  # gate" checkable rather than promised. The one thing A6 writes is a
  # revalidation record, and it goes through A4's single write point.
  src/db/db_gate.c
  src/gate/gate.c
  src/gate/assess.c
  src/core/service_gate.c
  # --- A7: operator authority --------------------------------------------
  #
  # There is deliberately no src/ipc file here either, and no MCP tool. A
  # profile's authority state is inspected, never set, and never over a
  # socket: a caller that could change what constrains it is not constrained.
  src/core/rootpath.c
  src/core/authority.c
  src/core/syspolicy.c
)

target_include_directories(atlas_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(atlas_core PRIVATE
  _POSIX_C_SOURCE=200809L
  ATLAS_BUILD_COMPILER="${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}"
)
target_link_libraries(atlas_core PUBLIC PkgConfig::SQLITE3 Threads::Threads)
target_link_libraries(atlas_core PRIVATE atlas_yyjson)
atlas_set_warnings(atlas_core)
if(ATLAS_SANITIZE_FLAGS)
  target_compile_options(atlas_core PUBLIC ${ATLAS_SANITIZE_FLAGS})
  target_link_options(atlas_core PUBLIC ${ATLAS_SANITIZE_FLAGS})
endif()

# --- executable -------------------------------------------------------------

add_executable(atlas src/main.c)
target_link_libraries(atlas PRIVATE atlas_core)
target_compile_definitions(atlas PRIVATE _POSIX_C_SOURCE=200809L)
atlas_set_warnings(atlas)

install(TARGETS atlas RUNTIME DESTINATION bin)

# The Claude Code marketplace and the plugin inside it are data, not build
# artefacts. The whole directory is installed — catalog and plugin together —
# because `claude plugin marketplace add` points at the catalog and the catalog
# refers to the plugin by a relative `./atlas` source, so the two have to keep
# their relationship on disk.
#
# It goes under <prefix>/share so it stays put: Claude copies an installed
# plugin into its own cache at a path that changes on every update, and nothing
# inside that copy can hold a stable path back to this binary. The launchers
# keep their executable bit through both copies.
install(DIRECTORY integrations/claude/
  DESTINATION share/atlas/claude-marketplace
  USE_SOURCE_PERMISSIONS
)

# --- tests ------------------------------------------------------------------

if(ATLAS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "Atlas: build type ${CMAKE_BUILD_TYPE}, asan=${ATLAS_ASAN}, ubsan=${ATLAS_UBSAN}")
