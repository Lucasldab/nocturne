# nocturne-tagcheck — Makefile
#
# Targets:
#   make tagcheck          build build/nocturne-tagcheck
#   make test              run the test suite (Phase 1 Plan 05 wires this up)
#   make clean             remove build outputs (preserves build/.gitkeep)
#   make help              short menu
#
# Toggles:
#   make tagcheck SAN=1    build with -fsanitize=address,undefined
#   make V=1               echo recipes (default: quiet)
#
# Plan 02 will add: TAGLIB_CFLAGS / TAGLIB_LIBS via pkg-config
# Plan 05 will replace the `test` target body with real test runners

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter -O2 -g
LDFLAGS ?=

BUILDDIR := build
BIN      := $(BUILDDIR)/nocturne-tagcheck

SRC_TAGCHECK := $(wildcard src/tagcheck/*.c)
OBJ_TAGCHECK := $(SRC_TAGCHECK:src/%.c=$(BUILDDIR)/%.o)

# Optional sanitizer build: SAN=1 → ASan + UBSan
ifeq ($(SAN),1)
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

# Quiet recipes by default; V=1 to expose commands.
Q := $(if $(V),,@)

# Dependency tracking
CFLAGS += -MMD -MP

.PHONY: all tagcheck test clean help

all: tagcheck

tagcheck: $(BIN)

test:
	@echo "tests not yet implemented (Phase 1 Plan 05)"

# Preserve build/.gitkeep so re-clones still have the empty dir tracked.
clean:
	$(Q)find $(BUILDDIR) -mindepth 1 ! -name .gitkeep -delete 2>/dev/null || true

help:
	@echo "Targets:"
	@echo "  tagcheck            build $(BIN)"
	@echo "  test                run the test suite (Plan 05)"
	@echo "  clean               remove build outputs (preserves .gitkeep)"
	@echo "  help                this menu"
	@echo "Toggles:"
	@echo "  SAN=1               -fsanitize=address,undefined"
	@echo "  V=1                 echo recipes"

# Compile rule. Order-only prereq on the per-package build dir.
$(BUILDDIR)/tagcheck/%.o: src/tagcheck/%.c | $(BUILDDIR)/tagcheck
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

# Link rule.
$(BIN): $(OBJ_TAGCHECK) | $(BUILDDIR)
	$(Q)$(CC) $(LDFLAGS) $(OBJ_TAGCHECK) -o $@

$(BUILDDIR)/tagcheck $(BUILDDIR):
	$(Q)mkdir -p $@

# Pull in auto-generated dependency files (header → object) when present.
-include $(OBJ_TAGCHECK:.o=.d)
