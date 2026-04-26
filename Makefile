# nocturne-tagcheck — Makefile
#
# Targets:
#   make tagcheck          build build/nocturne-tagcheck
#   make test              run the test suite (fixtures + per-suite binaries)
#   make clean             remove build outputs and fixture audio (preserves .gitkeep)
#   make help              short menu
#
# Toggles:
#   make tagcheck SAN=1    build with -fsanitize=address,undefined
#   make V=1               echo recipes (default: quiet)

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter -O2 -g
LDFLAGS ?=

BUILDDIR := build
BIN      := $(BUILDDIR)/nocturne-tagcheck

SRC_TAGCHECK := $(wildcard src/tagcheck/*.c)
OBJ_TAGCHECK := $(SRC_TAGCHECK:src/%.c=$(BUILDDIR)/%.o)

# TagLib via pkg-config — Arch package: extra/taglib (provides taglib_c.pc)
TAGLIB_CFLAGS := $(shell pkg-config --cflags taglib_c 2>/dev/null)
TAGLIB_LIBS   := $(shell pkg-config --libs taglib_c 2>/dev/null)

ifeq ($(TAGLIB_LIBS),)
$(error TagLib not found via pkg-config. Install taglib (Arch: pacman -S taglib))
endif

# Optional sanitizer build: SAN=1 → ASan + UBSan
ifeq ($(SAN),1)
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

# Quiet recipes by default; V=1 to expose commands.
Q := $(if $(V),,@)

# Dependency tracking
CFLAGS += -MMD -MP

# --- Test variables ---
TEST_SRC      := $(wildcard tests/test_*.c)
TEST_BINS     := $(TEST_SRC:tests/%.c=$(BUILDDIR)/tests/%)
TEST_RUNNER_O := $(BUILDDIR)/tests/runner.o
FIXTURES_DIR  := tests/fixtures

# Tests link the same .o files the main binary uses (minus main.o which has int main()).
LIB_OBJ := $(filter-out $(BUILDDIR)/tagcheck/main.o, $(OBJ_TAGCHECK))

.PHONY: all tagcheck test clean help fixtures

all: tagcheck

tagcheck: $(BIN)

# Test target: build fixtures, build per-suite binaries, run each.
test: $(FIXTURES_DIR)/.fixtures.stamp $(TEST_BINS)
	@echo "==> Running test suite"
	@failed=0; \
	for t in $(TEST_BINS); do \
	    echo "--- $$t"; \
	    if ! "$$t" $(FIXTURES_DIR); then failed=1; fi; \
	done; \
	if [ $$failed -ne 0 ]; then echo "==> Test suite FAILED"; exit 1; fi; \
	echo "==> Test suite PASSED"

# Fixture generation gate: regenerate only if the script is newer than the
# stamp file (or the stamp is missing).
$(FIXTURES_DIR)/.fixtures.stamp: tests/gen-fixtures.sh | $(FIXTURES_DIR)
	$(Q)bash tests/gen-fixtures.sh $(FIXTURES_DIR)
	$(Q)touch $@

fixtures: $(FIXTURES_DIR)/.fixtures.stamp

$(FIXTURES_DIR):
	$(Q)mkdir -p $@

# Preserve .gitkeep files at the leaves of build/ and tests/fixtures/.
clean:
	$(Q)find $(BUILDDIR) -mindepth 1 ! -name .gitkeep -delete 2>/dev/null || true
	$(Q)find $(FIXTURES_DIR) -mindepth 1 ! -name .gitkeep -delete 2>/dev/null || true

help:
	@echo "Targets:"
	@echo "  tagcheck            build $(BIN)"
	@echo "  test                build fixtures + run all tests/test_*.c suites"
	@echo "  fixtures            (re)generate audio fixtures only"
	@echo "  clean               remove build outputs and fixtures (preserves .gitkeep)"
	@echo "  help                this menu"
	@echo "Toggles:"
	@echo "  SAN=1               -fsanitize=address,undefined"
	@echo "  V=1                 echo recipes"

# --- Production build rules ---
$(BUILDDIR)/tagcheck/%.o: src/tagcheck/%.c | $(BUILDDIR)/tagcheck
	$(Q)$(CC) $(CFLAGS) $(TAGLIB_CFLAGS) -c $< -o $@

$(BIN): $(OBJ_TAGCHECK) | $(BUILDDIR)
	$(Q)$(CC) $(LDFLAGS) $(OBJ_TAGCHECK) $(TAGLIB_LIBS) -o $@

$(BUILDDIR)/tagcheck $(BUILDDIR):
	$(Q)mkdir -p $@

# --- Test build rules ---
$(BUILDDIR)/tests/runner.o: tests/runner.c tests/runner.h | $(BUILDDIR)/tests
	$(Q)$(CC) $(CFLAGS) -Itests -Isrc/tagcheck $(TAGLIB_CFLAGS) -c $< -o $@

$(BUILDDIR)/tests/test_%: tests/test_%.c $(TEST_RUNNER_O) $(LIB_OBJ) | $(BUILDDIR)/tests
	$(Q)$(CC) $(CFLAGS) -Itests -Isrc/tagcheck $(TAGLIB_CFLAGS) $(LDFLAGS) \
	    $< $(TEST_RUNNER_O) $(LIB_OBJ) $(TAGLIB_LIBS) -o $@

$(BUILDDIR)/tests:
	$(Q)mkdir -p $@

# Pull in auto-generated dependency files (header → object) when present.
-include $(OBJ_TAGCHECK:.o=.d)
-include $(TEST_BINS:%=%.d)
-include $(TEST_RUNNER_O:.o=.d)
