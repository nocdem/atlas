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

# libclang: the compiler-derived half of the code intelligence (A8-CI).
#
# Optional, deliberately. Atlas' rule is that a missing system dependency halts
# with a clear message, and that still holds for anything Atlas cannot work
# without — SQLite3 above is REQUIRED. The semantic index is different: it is a
# derived, rebuildable layer, and a build without it must still produce a binary
# that indexes, answers decisions and serves every A0..A8 command. What it must
# *not* do is answer a semantic question with an empty result, so the absence is
# carried into the binary as ATLAS_HAVE_LIBCLANG and reported by
# `atlas_sem_available()`, `atlas code status` and every entry point.
#
# Nothing is downloaded. This locates a library that is already installed, the
# same way SQLite3 is located.
set(ATLAS_LIBCLANG_ROOT "" CACHE PATH "Directory holding clang-c/Index.h and libclang")
find_path(ATLAS_LIBCLANG_INCLUDE_DIR clang-c/Index.h
  HINTS ${ATLAS_LIBCLANG_ROOT}/include
  PATHS /usr/lib/llvm-19/include /usr/lib/llvm-18/include /usr/lib/llvm-17/include
        /usr/lib/llvm-16/include /usr/lib/llvm-15/include /usr/lib/llvm-14/include
        /usr/include)
find_library(ATLAS_LIBCLANG_LIBRARY NAMES clang libclang
  HINTS ${ATLAS_LIBCLANG_ROOT}/lib
  PATHS /usr/lib/llvm-19/lib /usr/lib/llvm-18/lib /usr/lib/llvm-17/lib
        /usr/lib/llvm-16/lib /usr/lib/llvm-15/lib /usr/lib/llvm-14/lib)

if(ATLAS_LIBCLANG_INCLUDE_DIR AND ATLAS_LIBCLANG_LIBRARY)
  set(ATLAS_HAVE_LIBCLANG ON)
  message(STATUS "Atlas: libclang ${ATLAS_LIBCLANG_LIBRARY}")
  message(STATUS "Atlas: libclang headers ${ATLAS_LIBCLANG_INCLUDE_DIR}")
else()
  set(ATLAS_HAVE_LIBCLANG OFF)
  message(WARNING
    "Atlas: libclang was not found, so this build has NO compiler-aware semantic "
    "index. Every `atlas code`/`atlas context` semantic command will refuse with a "
    "typed error rather than return an empty result. Install the libclang "
    "development package (Debian/Ubuntu: libclang-dev) and reconfigure to enable it.")
endif()

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

# --- the embedded Mission Control page --------------------------------------
#
# A9 serves one page from the binary rather than from disk, so the gateway has
# no filesystem read path at all — which is what makes "no arbitrary filesystem
# access introduced by the gateway" a property of the code rather than a claim
# about a directory's permissions.
#
# The generator is first-party C, for the reason the JSON checker and the
# fixture generators are: the build depends on no language runtime. Not `xxd`,
# which is not guaranteed present; not a shell loop, because there is no shell
# in this build; and not Node, which the hard rules forbid outright — so the
# page is hand-written HTML, CSS and JavaScript with no bundler.

add_executable(atlas-embed tools/atlas_embed.c)
atlas_set_warnings(atlas-embed)

set(ATLAS_UI_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/gw/ui/mission-control.html)
set(ATLAS_UI_GEN ${CMAKE_CURRENT_BINARY_DIR}/atlas_ui_page.c)
add_custom_command(
  OUTPUT ${ATLAS_UI_GEN}
  COMMAND atlas-embed atlas_ui_page ${ATLAS_UI_SRC} ${ATLAS_UI_GEN}
  DEPENDS atlas-embed ${ATLAS_UI_SRC}
  COMMENT "Atlas: embedding the Mission Control page"
  VERBATIM
)

# --- core library -----------------------------------------------------------

add_library(atlas_core STATIC
  src/core/error.c
  src/core/buf.c
  src/core/scanner_uid.c
  src/core/mirror_open.c
  src/core/sha256.c
  src/core/hmac.c
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
  src/daemon/ops.c
  src/daemon/writer.c
  src/daemon/workers.c
  src/daemon/watch.c
  src/daemon/mirror.c
  src/daemon/scanner_seen.c
  src/daemon/daemon.c
  src/core/service_daemon.c
    src/core/service_remote.c
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
  # --- A8-CI: compiler-aware semantic code intelligence ------------------
  src/sem/sem.c
  src/sem/clangparse.c
  src/sem/parse.c
  src/db/db_sem.c
  src/sem/index.c
  src/sem/schedule.c
  src/sem/discover.c
  src/sem/query.c
  src/core/service_sem.c
  src/ipc/server_sem.c
  src/sem/context.c
  # --- A4: decision documents, revisions and operator approval -----------
  src/db/db_decision.c
  src/decision/decision.c
  src/decision/lifecycle.c
  src/core/terminal.c
  src/core/service_decision.c
  src/ipc/server_decision.c
  # --- A5: operational backup, verification, restore and maintenance -----
  #
  # Restore and maintenance are local CLI operations with no RPC method, so
  # nothing reachable from MCP or a hook can replace or prune the index.
  #
  # Backup create and verify do have one, added when A7.1's 0700 index made
  # the operator account the one account that could not read its own backup.
  # They live in the operator-gated group in src/ipc/server_backup.c, behind
  # the SO_PEERCRED test that gates approve and reject — not in the ordinary
  # client surface, and restore is still absent from the protocol entirely.
  src/db/db_backup.c
  src/core/service_backup.c
  src/ipc/server_backup.c
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
  # --- A8: the durable orchestration control plane ------------------------
  #
  # `atlasd` owns every orchestration row and is the only writer of one;
  # `atlas-worker` runs the dispatcher and holds no database handle at all.
  # The split is enforced by the filesystem first (0700 atlasd on the index)
  # and by these files second, which is the ordering A7.1 established.
  #
  # Nothing here can approve a decision, change the registry or read a
  # backup. A completed job is an artifact and a history, never an authority.
  src/orch/orch.c
  src/orch/policy.c
  src/orch/workspace.c
  src/orch/driver.c
  src/orch/validate.c
  src/orch/rundriver.c
  src/orch/usage.c
  src/orch/memory.c
  src/orch/dispatch.c
  src/orch/snapshot.c
  # A12.0: the `atlas-plan-1` format. Pure — no database handle, no process, no
  # clock — and it compiles a proposal into nothing more than task rows.
  src/orch/plan.c
  # A12.0: the foreground plan driver. One loop an operator starts, above the
  # same transport the run driver uses. No thread, no timer, no polling, no
  # scheduler, and no state that must survive a crash.
  src/orch/plandriver.c
  src/core/service_orch.c
  src/core/service_scanner.c
  # A12.0: the four `plan.` calls a client makes. No offline path, no second
  # derivation of a plan's status, and no formatting.
  src/core/service_plan.c
  src/db/db_orch.c
  src/db/db_orch_memory.c
  # A12.0: migration 25's three tables, their one write point, and the plan
  # status that is derived on every read because nothing writes one.
  src/db/db_plan.c
  src/ipc/server_orch.c
  src/ipc/server_scanner.c
  # A12.1 T11. memory.put, memory.status and memory.reconcile -- the operator
  # group beside repo.scanner, above.
  src/ipc/server_memory.c
  # --- A9: remote access, API credentials and the web control plane -------
  #
  # The gateway is a *client* of the daemon, structurally the same shape as
  # the MCP adapter: it holds no database handle, opens no repository and
  # creates no process. Every answer it gives came over the Unix socket, and
  # the account it runs as is neither the operator uid nor a dispatcher uid,
  # so the kernel — not a check in C — is what stops it approving a decision,
  # changing the registry, reading a backup or running a job.
  #
  # `apikey.c` holds the credential format and the verifier; nothing in it
  # touches a database. `hmac.c` is in src/core because it is a primitive,
  # not a gateway concern.
  src/gw/apikey.c
  src/db/db_gw.c
  src/core/service_apikey.c
  src/gw/gwpolicy.c
  src/gw/http.c
  src/ipc/server_apikey.c
  src/ipc/server_gw.c
  src/gw/gateway.c

  # --- A9.2: claims, attestations, evidence and self-verifying memory -------
  #
  # `verify.c` is the vocabularies and the aggregation algorithm and touches no
  # database: it is a pure function of counted inputs, which is what lets the
  # scoring be tested without one. `db_verify.c` is the single write point over
  # the migration-14 tables. `policy.c` loads the root-owned verification
  # policy. `detverify.c` runs the deterministic verifiers, every one of which
  # is a read. `autolifecycle.c` is the only thing that turns a verification
  # result into a lifecycle transition, and it owns its transaction because the
  # audit row and the transition must commit together.
  src/verify/verify.c
  src/core/service_verify.c
  src/verify/policy.c
  src/db/db_verify.c
  src/verify/detverify.c
  src/verify/autolifecycle.c
  # A9.2.1. `intake.c` is the one write point for a claim, a piece of evidence,
  # an attestation and a dependency edge — the half A9.2 shipped without, which
  # left the engine above unable to be fed by anything but its own tests. The
  # channel check, the actor derivation, the forged-producer refusal, the
  # reference validation and the content-key idempotency all live behind it.
  src/verify/intake.c
  src/ipc/server_verify.c

  # --- A12.1: reconciled model memory ---------------------------------------
  #
  # `source.c` is the five vocabularies and the policy value grammar that
  # registers a memory source. Pure: no database handle, no process, no file and
  # no clock, which is what lets the whole grammar be enumerated by a unit test
  # rather than only through a root-owned file no test process can create.
  # `db_memory.c` is the one surface over migration 29's tables — its
  # functions are the only production writers of a `memory_*` table,
  # `db_verify.c`'s shape for `db_verify.c`'s reason.
  src/memory/source.c
  src/db/db_memory.c
  # `read.c` (T6) turns a registered source into the bytes it names, by
  # asking A13's own routing (atlas_repo_open_git) rather than restating it —
  # it never opens a registered tree itself and never falls back to one.
  src/memory/read.c
  # `extract.c` (T7) splits a source's bytes into candidate propositions —
  # pure, `src/orch/memory.c`'s discipline — and resolves their anchors
  # against the index, which is the impure half T8's write transaction calls.
  src/memory/extract.c
  # `reconcile.c` (T8) is the pass: observe outside a transaction, apply
  # inside one. It creates no `verify_*` row itself — every one goes through
  # `atlas_verify_intake_apply_in_tx` — and it is the first real consumer of
  # T4's DOCUMENT channel and T5's conflict producer.
  src/memory/reconcile.c
  # `pack.c` (T12) is the Canonical Context Pack: build, freeze, freshness,
  # render, compose. Build reads stored rows plus, when the repository is
  # dirty, one `atlas_sem_source_identity` call that opens the tree, so it
  # must run outside any transaction; freeze inserts an already-built struct
  # and reads nothing, so it runs inside one, beside `atlas_db_orch_memory_
  # freeze`. It issues no SQL of its own — every read and the one write go
  # through db_memory.c/db_verify.c, so T17's rule that a memory-table row
  # insert lives only in db_memory.c holds by construction.
  src/memory/pack.c
  # `trailer.c` (T14) is the commit trailer: composed for a person from a
  # frozen pack's own stored identifiers, ingested from `commits.body` (no
  # process, no git invocation). Every field that does not verify against
  # Atlas' own rows is named in `unknown_fields` and binds nothing -- a
  # trailer is a pointer, never proof and never authority.
  src/memory/trailer.c
  # `patch.c` (T15) is the proposed patch: a unified diff against one
  # registered source's current content, proposing only deletions, plus a
  # findings list for everything else. Reads a source through the same T6
  # path pack.c's build phase already uses (`atlas_memory_read_source`,
  # A13-routed) and reads (never writes) `verify_*`/`memory_*` rows to decide
  # what qualifies -- it opens no transaction, so it must run outside one,
  # `atlas_memory_pack_build`'s own rule.
  src/memory/patch.c
  # A12.1 T16. The `memory` command family's one service file: seven forms,
  # one render struct (`atlas_memory_render`), one sink. `status` reads
  # locally via `atlas_ctx_db(ctx)` when it can and falls back to the
  # existing `memory.status` operator method otherwise; `scan` and
  # `reconcile` always speak to the daemon, because the writer-thread job
  # queue they submit to has no local equivalent. `pack`, `diff`, `patch`
  # and `trailer` are local-only in this build — see the file's own header
  # comment for the disclosed A7.1 gap.
  src/core/service_memory.c

  # --- A15 T3: the review sheet grammar --------------------------------------
  #
  # The grammar and the vocabulary only: bytes in, a parsed sheet or a refusal
  # out. Pure, `src/memory/source.c`'s discipline -- no database handle, no
  # process, no file, no clock -- which is what lets the whole grammar be
  # enumerated by a unit test rather than only exercised through a real
  # terminal session a later task's walker drives.
  src/core/review.c
  # --- A15 T4: the review walker ----------------------------------------------
  #
  # Loops atlas_service_decision_confirm -- the operator channel's one entry
  # point -- and adds no other way to reach it: the pre-check that decides
  # whether to call it at all is composed entirely out of existing service
  # functions (atlas_service_decision_show and its remote form), so this file
  # opens no database handle and creates no process of its own.
  src/core/service_review.c
  ${ATLAS_UI_GEN}
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
if(ATLAS_HAVE_LIBCLANG)
  # PRIVATE, and only src/sem/clangparse.c ever includes the header: libclang is
  # reachable from exactly one translation unit, which is what keeps "Atlas links
  # a compiler library in one place" checkable rather than asserted.
  target_compile_definitions(atlas_core PRIVATE ATLAS_HAVE_LIBCLANG=1)
  target_include_directories(atlas_core PRIVATE ${ATLAS_LIBCLANG_INCLUDE_DIR})
  target_link_libraries(atlas_core PRIVATE ${ATLAS_LIBCLANG_LIBRARY})
endif()
atlas_set_warnings(atlas_core)
if(ATLAS_SANITIZE_FLAGS)
  target_compile_options(atlas_core PUBLIC ${ATLAS_SANITIZE_FLAGS})
  target_link_options(atlas_core PUBLIC ${ATLAS_SANITIZE_FLAGS})
  # Tells src/core/proc.c to leave the child address-space ceiling off: a
  # sanitizer reserves terabytes of shadow space, so any RLIMIT_AS an ordinary
  # workload wants kills the child before main. See the comment there.
  target_compile_definitions(atlas_core PUBLIC ATLAS_SANITIZER_BUILD=1)
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
