# Compiler and flags
#   := is immediate assignment — evaluated once when make reads the file
#   -O2            optimize
#   -Wall -Wextra  enable most warnings
#   -std=c11       compile as C11
#   -D_GNU_SOURCE  expose POSIX/GNU extensions (needed for CLOCK_MONOTONIC, struct timeval, etc.)
CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE

# Output directory for compiled binaries
BUILD   := build

# $(wildcard v*/) — glob expands at make-time; automatically picks up v2_*, v3_*, etc.
VERSIONS := $(wildcard v*/)

# $(foreach d,VERSIONS,...) — loops over each version dir and collects all .c files
# used by the fmt and lint targets
SRCS     := $(foreach d,$(VERSIONS),$(wildcard $(d)*.c)) benchmark.c

# same foreach loop but produces expected binary output paths, e.g.:
#   build/v1_blocking/server  build/v1_blocking/client
# used by the all target — make rebuilds a binary if it is missing or older than its source
BINS     := $(foreach d,$(VERSIONS),$(BUILD)/$(d)server $(BUILD)/$(d)client) $(BUILD)/benchmark

# --------------------------------------------------------------------------- #
# Build                                                                        #
# --------------------------------------------------------------------------- #

.PHONY: all
all: $(BINS)

# Generate explicit rules for each versioned directory
define MAKE_RULES
$(BUILD)/$(1)server: $(1)server.c
	mkdir -p $(BUILD)/$(1)
	$(CC) $(CFLAGS) -o $$@ $$<

$(BUILD)/$(1)client: $(1)client.c
	mkdir -p $(BUILD)/$(1)
	$(CC) $(CFLAGS) -o $$@ $$<
endef

$(foreach d,$(VERSIONS),$(eval $(call MAKE_RULES,$(d))))

# benchmark lives at the root, links with pthreads
$(BUILD)/benchmark: benchmark.c
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< -lpthread

# --------------------------------------------------------------------------- #
# Format                                                                       #
# --------------------------------------------------------------------------- #

.PHONY: fmt fmt-check
fmt:
	clang-format -i $(SRCS)

fmt-check:
	clang-format --dry-run --Werror $(SRCS)

# --------------------------------------------------------------------------- #
# Lint                                                                         #
# --------------------------------------------------------------------------- #

.PHONY: lint
lint:
	@for src in $(SRCS); do \
		echo "==> $$src"; \
		clang-tidy $$src -- $(CFLAGS); \
	done

# --------------------------------------------------------------------------- #
# Clean                                                                        #
# --------------------------------------------------------------------------- #

.PHONY: clean
clean:
	rm -rf $(BUILD)
