# Atlas - convenience wrapper around the canonical CMake build.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
#   make            release build            -> build/atlas
#   make debug      debug build              -> build-debug/atlas
#   make test       build + full CTest suite
#   make smoke      build + CLI smoke test (compiled JSON checker, no Python)
#   make adversarial  build + hostile-repository hardening checks under strace
#   make asan       ASan/LSan build + tests  -> build-asan
#   make ubsan      UBSan build + tests      -> build-ubsan
#   make tsan       ThreadSanitizer build + tests -> build-tsan
#   make verify-vendor  re-check vendored third-party digests
#   make install    install to $(PREFIX)/bin (default /usr/local/bin)
#   make clean      remove all build directories

CMAKE   ?= cmake
CTEST   ?= ctest
PREFIX  ?= /usr/local
JOBS    ?= $(shell nproc 2>/dev/null || echo 2)

BUILD_RELEASE ?= build
BUILD_DEBUG   ?= build-debug
BUILD_ASAN    ?= build-asan
BUILD_UBSAN   ?= build-ubsan
BUILD_TSAN    ?= build-tsan

CTEST_FLAGS ?= --output-on-failure

.PHONY: all release debug test test-debug smoke adversarial asan ubsan tsan verify-vendor install clean distclean doctor doctor-claude claude-install-test compiledb help

all: release

release:
	$(CMAKE) -S . -B $(BUILD_RELEASE) -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build $(BUILD_RELEASE) -j $(JOBS)

debug:
	$(CMAKE) -S . -B $(BUILD_DEBUG) -DCMAKE_BUILD_TYPE=Debug
	$(CMAKE) --build $(BUILD_DEBUG) -j $(JOBS)

test: release
	cd $(BUILD_RELEASE) && $(CTEST) $(CTEST_FLAGS) -j $(JOBS)

# CLI smoke test. Uses the compiled JSON checker, not any language runtime.
smoke: release
	./scripts/smoke.sh $(BUILD_RELEASE)

# Adversarial git-hardening verification under strace.
adversarial: release
	./scripts/adversarial.sh $(BUILD_RELEASE)

test-debug: debug
	cd $(BUILD_DEBUG) && $(CTEST) $(CTEST_FLAGS) -j $(JOBS)

asan:
	$(CMAKE) -S . -B $(BUILD_ASAN) -DCMAKE_BUILD_TYPE=Debug -DATLAS_ASAN=ON
	$(CMAKE) --build $(BUILD_ASAN) -j $(JOBS)
	cd $(BUILD_ASAN) && ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 $(CTEST) $(CTEST_FLAGS) -j $(JOBS)

ubsan:
	$(CMAKE) -S . -B $(BUILD_UBSAN) -DCMAKE_BUILD_TYPE=Debug -DATLAS_UBSAN=ON
	$(CMAKE) --build $(BUILD_UBSAN) -j $(JOBS)
	cd $(BUILD_UBSAN) && UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 $(CTEST) $(CTEST_FLAGS) -j $(JOBS)

# ThreadSanitizer, for the A1 threaded components. TSan and ASan cannot be
# combined, so this is its own build directory.
tsan:
	$(CMAKE) -S . -B $(BUILD_TSAN) -DCMAKE_BUILD_TYPE=Debug -DATLAS_TSAN=ON
	$(CMAKE) --build $(BUILD_TSAN) -j $(JOBS)
	cd $(BUILD_TSAN) && TSAN_OPTIONS=halt_on_error=0:second_deadlock_stack=1 $(CTEST) $(CTEST_FLAGS) -j $(JOBS)

# Vendored third-party source must still be what PROVENANCE.md says it is.
verify-vendor:
	./scripts/verify_third_party.sh

install: release
	$(CMAKE) --install $(BUILD_RELEASE) --prefix $(PREFIX)

# Reports the environment without touching it. `atlas doctor` observes in
# ATLAS_CTX_INSPECT mode: it creates no data directory, no database, no lock and
# no socket, so this is safe to run on a machine where Atlas has never been used
# — which is exactly when somebody wants to run it.
#
# It deliberately does NOT pass --data-dir. The whole point is to report on the
# data directory the user's environment resolves to, and reporting on a
# throwaway one would answer a different question.
doctor: release
	./$(BUILD_RELEASE)/atlas doctor

# The same, plus the AI integration. Also side-effect free.
doctor-claude: release
	./$(BUILD_RELEASE)/atlas integrate claude doctor

# Drives the real `claude` CLI through the documented permanent installation —
# marketplace add, install at user scope, list, uninstall — entirely inside a
# temporary HOME and CLAUDE_CONFIG_DIR. Skips cleanly when claude is absent,
# because Claude is not a build dependency of Atlas.
#
# Not part of `make test`: it needs a program the build does not require, and a
# suite that silently skips is a suite people stop reading.
claude-install-test: release
	sh scripts/claude-install-test.sh $(BUILD_RELEASE)

# Refresh the top-level compile_commands.json symlink for editors and clangd.
compiledb: release
	ln -sf $(BUILD_RELEASE)/compile_commands.json compile_commands.json

clean:
	rm -rf $(BUILD_RELEASE) $(BUILD_DEBUG) $(BUILD_ASAN) $(BUILD_UBSAN) $(BUILD_TSAN)

distclean: clean
	rm -f compile_commands.json

help:
	@sed -n '3,14p' Makefile
