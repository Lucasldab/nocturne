# nocturne — Makefile
#
# Targets:
#   make tagcheck          build build/nocturne-tagcheck (Phase 1)
#   make nocturned         build build/nocturned        (Phase 2+)
#   make test              run the test suite (fixtures + per-suite binaries)
#   make clean             remove build outputs and fixture audio (preserves .gitkeep)
#   make help              short menu
#
# Toggles:
#   make ... SAN=1         build with -fsanitize=address,undefined
#   make V=1               echo recipes (default: quiet)

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter -O2 -g
LDFLAGS ?=

BUILDDIR := build
BIN      := $(BUILDDIR)/nocturne-tagcheck
BIN_NOCTURNED := $(BUILDDIR)/nocturned

SRC_TAGCHECK := $(wildcard src/tagcheck/*.c)
OBJ_TAGCHECK := $(SRC_TAGCHECK:src/%.c=$(BUILDDIR)/%.o)

SRC_NOCTURNED := $(wildcard src/nocturned/*.c)
# Object directory name avoids colliding with the BIN_NOCTURNED file path.
OBJ_NOCTURNED := $(SRC_NOCTURNED:src/nocturned/%.c=$(BUILDDIR)/nocturned-obj/%.o)

# Vendored SHA-256 (public domain, kept under vendor/sha256/) — links into
# the daemon and into nocturned-suite tests so we don't need OpenSSL.
SRC_VENDOR_SHA256 := vendor/sha256/sha256.c
OBJ_VENDOR_SHA256 := $(BUILDDIR)/vendor/sha256/sha256.o

# TagLib via pkg-config — Arch package: extra/taglib (provides taglib_c.pc)
TAGLIB_CFLAGS := $(shell pkg-config --cflags taglib_c 2>/dev/null)
TAGLIB_LIBS   := $(shell pkg-config --libs taglib_c 2>/dev/null)

ifeq ($(TAGLIB_LIBS),)
$(error TagLib not found via pkg-config. Install taglib (Arch: pacman -S taglib))
endif

# SQLite via pkg-config — Arch package: core/sqlite
SQLITE_CFLAGS := $(shell pkg-config --cflags sqlite3 2>/dev/null)
SQLITE_LIBS   := $(shell pkg-config --libs sqlite3 2>/dev/null)

ifeq ($(SQLITE_LIBS),)
$(error SQLite3 not found via pkg-config. Install sqlite (Arch: pacman -S sqlite))
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
LIB_OBJ           := $(filter-out $(BUILDDIR)/tagcheck/main.o, $(OBJ_TAGCHECK))
LIB_OBJ_NOCTURNED := $(filter-out $(BUILDDIR)/nocturned-obj/main.o, $(OBJ_NOCTURNED))

# Tagcheck-suite tests vs nocturned-suite tests — split so each set links the
# right .o pile without fighting include paths or duplicate symbols.
# Membership is computed from existing test sources so a half-built tree
# (e.g. plan 02-01 before db/lock land) still has a valid `make test`.
TAGCHECK_TEST_SRC_NAMES  := $(notdir $(wildcard tests/test_walker.c tests/test_check.c tests/test_quarantine.c))
NOCTURNED_TEST_SRC_NAMES := $(notdir $(wildcard tests/test_db.c tests/test_lock.c tests/test_hash.c tests/test_scan.c))

TAGCHECK_TEST_BINS  := $(TAGCHECK_TEST_SRC_NAMES:%.c=$(BUILDDIR)/tests/%)
NOCTURNED_TEST_BINS := $(NOCTURNED_TEST_SRC_NAMES:%.c=$(BUILDDIR)/tests/%)

ALL_TEST_BINS := $(TAGCHECK_TEST_BINS) $(NOCTURNED_TEST_BINS)

.PHONY: all tagcheck nocturned test clean help fixtures

all: tagcheck nocturned

tagcheck: $(BIN)
nocturned: $(BIN_NOCTURNED)

# Test target: build fixtures, build per-suite binaries, run each.
test: $(FIXTURES_DIR)/.fixtures.stamp $(ALL_TEST_BINS)
	@echo "==> Running test suite"
	@failed=0; \
	for t in $(ALL_TEST_BINS); do \
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
	$(Q)rm -f src/nocturned/_schema_*.h

help:
	@echo "Targets:"
	@echo "  tagcheck            build $(BIN)"
	@echo "  nocturned           build $(BIN_NOCTURNED)"
	@echo "  test                build fixtures + run all tests/test_*.c suites"
	@echo "  fixtures            (re)generate audio fixtures only"
	@echo "  clean               remove build outputs and fixtures (preserves .gitkeep)"
	@echo "  help                this menu"
	@echo "Toggles:"
	@echo "  SAN=1               -fsanitize=address,undefined"
	@echo "  V=1                 echo recipes"

# --- Tagcheck production build rules ---
$(BUILDDIR)/tagcheck/%.o: src/tagcheck/%.c | $(BUILDDIR)/tagcheck
	$(Q)$(CC) $(CFLAGS) $(TAGLIB_CFLAGS) -c $< -o $@

$(BIN): $(OBJ_TAGCHECK) | $(BUILDDIR)
	$(Q)$(CC) $(LDFLAGS) $(OBJ_TAGCHECK) $(TAGLIB_LIBS) -o $@

# --- Nocturned schema embedding ---
# Generated headers (xxd -i schema/NNNN_*.sql) live next to the C sources
# so they're picked up by `-Isrc/nocturned`. They are .gitignored.
SCHEMA_SQL    := $(wildcard schema/*.sql)
SCHEMA_HDRS   := $(SCHEMA_SQL:schema/%.sql=src/nocturned/_schema_%.h)

# `cd schema` keeps the xxd-generated symbol name short (just NNNN_init_sql)
# instead of including the directory path. xxd prepends `__` automatically
# when the filename starts with a digit; migrations.c depends on that.
src/nocturned/_schema_%.h: schema/%.sql
	$(Q)cd schema && xxd -i $*.sql > ../src/nocturned/_schema_$*.h

# --- Nocturned production build rules ---
$(BUILDDIR)/nocturned-obj/migrations.o: $(SCHEMA_HDRS)

$(BUILDDIR)/nocturned-obj/%.o: src/nocturned/%.c | $(BUILDDIR)/nocturned-obj
	$(Q)$(CC) $(CFLAGS) $(SQLITE_CFLAGS) $(TAGLIB_CFLAGS) -Isrc -Isrc/tagcheck -c $< -o $@

$(BIN_NOCTURNED): $(OBJ_NOCTURNED) $(OBJ_VENDOR_SHA256) $(LIB_OBJ) | $(BUILDDIR)
	$(Q)$(CC) $(LDFLAGS) $(OBJ_NOCTURNED) $(OBJ_VENDOR_SHA256) $(LIB_OBJ) $(SQLITE_LIBS) $(TAGLIB_LIBS) -o $@

# Vendored sha256 build rule.
$(BUILDDIR)/vendor/sha256/sha256.o: $(SRC_VENDOR_SHA256) | $(BUILDDIR)/vendor/sha256
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/vendor/sha256:
	$(Q)mkdir -p $@

$(BUILDDIR)/tagcheck $(BUILDDIR)/nocturned-obj $(BUILDDIR):
	$(Q)mkdir -p $@

# --- Test build rules ---
$(BUILDDIR)/tests/runner.o: tests/runner.c tests/runner.h | $(BUILDDIR)/tests
	$(Q)$(CC) $(CFLAGS) -Itests -Isrc/tagcheck $(TAGLIB_CFLAGS) -c $< -o $@

# Tagcheck tests: link tagcheck library objects + taglib.
$(TAGCHECK_TEST_BINS): $(BUILDDIR)/tests/%: tests/%.c $(TEST_RUNNER_O) $(LIB_OBJ) | $(BUILDDIR)/tests
	$(Q)$(CC) $(CFLAGS) -Itests -Isrc/tagcheck $(TAGLIB_CFLAGS) $(LDFLAGS) \
	    $< $(TEST_RUNNER_O) $(LIB_OBJ) $(TAGLIB_LIBS) -o $@

# Nocturned tests: link nocturned library objects + vendored sha256 + sqlite3.
# Tagcheck library objects are also linked because scan_*.c and the canonical-
# tag helpers borrow Phase 1's tags.o / walker.o / check.o symbols directly.
$(NOCTURNED_TEST_BINS): $(BUILDDIR)/tests/%: tests/%.c $(TEST_RUNNER_O) $(LIB_OBJ_NOCTURNED) $(OBJ_VENDOR_SHA256) $(LIB_OBJ) | $(BUILDDIR)/tests
	$(Q)$(CC) $(CFLAGS) -Itests -Isrc/nocturned -Isrc -Isrc/tagcheck $(SQLITE_CFLAGS) $(TAGLIB_CFLAGS) $(LDFLAGS) \
	    $< $(TEST_RUNNER_O) $(LIB_OBJ_NOCTURNED) $(OBJ_VENDOR_SHA256) $(LIB_OBJ) $(SQLITE_LIBS) $(TAGLIB_LIBS) -o $@

$(BUILDDIR)/tests:
	$(Q)mkdir -p $@

# Pull in auto-generated dependency files (header → object) when present.
-include $(OBJ_TAGCHECK:.o=.d)
-include $(OBJ_NOCTURNED:.o=.d)
-include $(ALL_TEST_BINS:%=%.d)
-include $(TEST_RUNNER_O:.o=.d)
