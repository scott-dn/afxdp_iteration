# Compiler and flags
#   := is immediate assignment — evaluated once when make reads the file
#   -O2            optimize
#   -Wall -Wextra  enable most warnings
#   -std=c11       compile as C11
#   -D_GNU_SOURCE  expose POSIX/GNU extensions (needed for CLOCK_MONOTONIC, struct timeval, etc.)
# LDLIBS is appended after the source file in the link command — required because
# the linker only pulls in symbols from a library to satisfy *already-seen* undefined
# references. -l flags placed before the .c file (e.g. in CFLAGS) get discarded.
#   -lpthread      libpthread — no-op for binaries that don't use it
#   -luring        liburing for v5; --as-needed drops it from v1–v4 binaries
CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE
LDLIBS  := -lpthread -luring

# Output directory for compiled binaries
BUILD   := build

# $(wildcard v*/) — glob expands at make-time; automatically picks up v2_*, v3_*, etc.
VERSIONS := $(wildcard v*/)

# loops over each version dir and collects all .c files
# used by the fmt and lint targets
SRCS     := $(foreach d,$(VERSIONS),$(wildcard $(d)*.c)) benchmark.c

# same foreach loop but produces expected binary output paths, e.g.:
#   build/v1_blocking_st/server  build/v2_blocking_mt/server
# used by the all target — make rebuilds a binary if it is missing or older than its source
BINS     := $(foreach d,$(VERSIONS),$(BUILD)/$(d)server) $(BUILD)/benchmark

# --------------------------------------------------------------------------- #
# Build                                                                       #
# --------------------------------------------------------------------------- #
.PHONY: all
all: $(BINS)

# Generate one server build rule per versioned directory.
# $(1) expands to the dir with trailing slash, e.g. v1_blocking_st/
define MAKE_RULES
$(BUILD)/$(1)server: $(1)server.c
	mkdir -p $(BUILD)/$(1)
	$(CC) $(CFLAGS) -o $$@ $$< $(LDLIBS)
endef

$(foreach d,$(VERSIONS),$(eval $(call MAKE_RULES,$(d))))

# benchmark lives at the repo root; same CFLAGS + LDLIBS as the servers.
$(BUILD)/benchmark: benchmark.c
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# Convenience alias: `make benchmark` → `make build/benchmark`.
# Without this, Make's implicit rule (%: %.c) would compile benchmark.c
# into ./benchmark at the repo root, leaking a stray binary outside build/.
.PHONY: benchmark
benchmark: $(BUILD)/benchmark

# --------------------------------------------------------------------------- #
# Format                                                                      #
# --------------------------------------------------------------------------- #
.PHONY: fmt fmt-check
fmt:
	clang-format -i $(SRCS)

fmt-check:
	clang-format --dry-run --Werror $(SRCS)

# --------------------------------------------------------------------------- #
# Lint                                                                        #
# --------------------------------------------------------------------------- #
.PHONY: lint
lint:
	@for src in $(SRCS); do \
		echo "==> $$src"; \
		clang-tidy $$src -- $(CFLAGS); \
	done

# --------------------------------------------------------------------------- #
# Clean                                                                       #
# --------------------------------------------------------------------------- #
.PHONY: clean
clean:
	rm -rf $(BUILD)
