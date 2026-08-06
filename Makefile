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

CTEST_FLAGS ?= --output-on-failure

.PHONY: all release debug test test-debug smoke adversarial asan ubsan install clean distclean doctor compiledb help

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

install: release
	$(CMAKE) --install $(BUILD_RELEASE) --prefix $(PREFIX)

doctor: release
	./$(BUILD_RELEASE)/atlas doctor

# Refresh the top-level compile_commands.json symlink for editors and clangd.
compiledb: release
	ln -sf $(BUILD_RELEASE)/compile_commands.json compile_commands.json

clean:
	rm -rf $(BUILD_RELEASE) $(BUILD_DEBUG) $(BUILD_ASAN) $(BUILD_UBSAN)

distclean: clean
	rm -f compile_commands.json

help:
	@sed -n '3,12p' Makefile
