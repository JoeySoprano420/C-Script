# =========================
# C-Script Makefile
# =========================
# Usage:
#   make                      # build cscriptc (system CC path by default)
#   make EMBED=1              # build cscriptc with embedded LLVM+LLD
#   make EMBED=1 PGOEMBED=1   # embedded LLVM + in-proc IR-level profiling pass
#   make examples             # build all .csc under examples/ to .exe
#   make run-examples         # build + run all examples
#   make clean                # remove outputs
#   make install PREFIX=/usr/local
#
# Build single .csc:
#   make hello.exe            # given hello.csc in cwd
#   make run-hello            # compiles hello.csc -> hello.exe and runs it
#
# Notes:
# - On Linux/macOS this uses `llvm-config` to find LLVM flags if EMBED=1.
# - If your distro splits clang/lld libs, you may need EXTRA_LLVM_LIBS=...
#   Example:
#     make EMBED=1 EXTRA_LLVM_LIBS="-lclangFrontend -lclangDriver ... -llldELF -llldCOFF -llldMachO -llldCommon"
#
# - On Windows, prefer MSYS2/Clang or WSL for this Makefile; the .cpp also supports MSVC.

# ---- User knobs --------------------------------------------------------------
CXX       ?= clang++
STD       ?= -std=c++17
OPT       ?= -O2
WARN      ?= -Wall -Wextra
DEBUG     ?=
PREFIX    ?= /usr/local
BINDIR    ?= $(PREFIX)/bin

# Enable embedded clang+lld in-proc compilation/link
EMBED     ?= 0
# Enable in-proc IR-level PGO (requires EMBED=1)
PGOEMBED  ?= 0

# Extra include/lib paths if you keep headers/libs in custom places
EXTRA_INC ?=
EXTRA_LIB ?=
# If your environment needs explicit clang/lld libs:
EXTRA_LLVM_LIBS ?=

# ---- Host & tools ------------------------------------------------------------
UNAME_S   := $(shell uname -s 2>/dev/null || echo Unknown)
LLVM_CONFIG ?= llvm-config
HAVE_LLVM  := $(shell $(LLVM_CONFIG) --version >/dev/null 2>&1 && echo 1 || echo 0)

# ---- Sources / Outputs -------------------------------------------------------
CSCRIPT_CPP   := cscriptc.cpp
CSCRIPT_BIN   := cscriptc

# Examples folder (optional)
EXDIR         ?= examples
EX_SOURCES    := $(wildcard $(EXDIR)/*.csc)
EX_BINS       := $(patsubst %.csc,%.exe,$(EX_SOURCES))

# ---- Base flags --------------------------------------------------------------
CXXFLAGS_BASE := $(STD) $(OPT) $(WARN) $(DEBUG) $(EXTRA_INC)
LDFLAGS_BASE  := $(EXTRA_LIB)

DEFS :=

# ---- Embedded LLVM / PGO wiring ---------------------------------------------
ifeq ($(EMBED),1)
  ifeq ($(HAVE_LLVM),1)
    DEFS           += -DCS_EMBED_LLVM=1
    LLVM_CXXFLAGS  := $(shell $(LLVM_CONFIG) --cxxflags)
    LLVM_LDFLAGS   := $(shell $(LLVM_CONFIG) --ldflags --system-libs)
    LLVM_LIBS_ALL  := $(shell $(LLVM_CONFIG) --libs all)

    # Common Clang + LLD libs (adjust if your install differs)
    CLANG_LIBS_DEF := -lclangFrontend -lclangDriver -lclangCodeGen -lclangParse -lclangSema \
                      -lclangSerialization -lclangAST -lclangLex -lclangBasic \
                      -lclangToolingCore -lclangRewrite -lclangARCMigrate
    LLD_LIBS_DEF   := -llldELF -llldCOFF -llldMachO -llldCommon

    CXXFLAGS := $(CXXFLAGS_BASE) $(LLVM_CXXFLAGS) $(DEFS)
    LDFLAGS  := $(LDFLAGS_BASE)  $(LLVM_LDFLAGS) $(LLVM_LIBS_ALL) \
                $(CLANG_LIBS_DEF) $(LLD_LIBS_DEF) $(EXTRA_LLVM_LIBS)
  else
    $(warning EMBED=1 requested but llvm-config not found; falling back to system-CC mode)
    CXXFLAGS := $(CXXFLAGS_BASE) $(DEFS)
    LDFLAGS  := $(LDFLAGS_BASE)
  endif

  ifeq ($(PGOEMBED),1)
    DEFS += -DCS_PGO_EMBED=1
  endif
else
  # System toolchain mode (no embedded LLVM)
  CXXFLAGS := $(CXXFLAGS_BASE) $(DEFS)
  LDFLAGS  := $(LDFLAGS_BASE)
endif

# ---- OS-specific tweaks ------------------------------------------------------
ifeq ($(UNAME_S),Darwin)
  # macOS: prefer libc++ with clang++
  CXXFLAGS += -stdlib=libc++
  LDFLAGS  += -lc++
endif

# ==== Targets =================================================================

.PHONY: all help clean install uninstall examples run-examples test env

all: $(CSCRIPT_BIN)

help:
	@echo "C-Script Makefile"
	@echo "  make                       # build cscriptc"
	@echo "  make EMBED=1               # build with embedded LLVM+LLD"
	@echo "  make EMBED=1 PGOEMBED=1    # embedded + IR-level PGO"
	@echo "  make examples              # build all examples/*.csc -> .exe"
	@echo "  make run-examples          # build and run all example exes"
	@echo "  make clean                 # remove outputs"
	@echo "  make install PREFIX=/usr/local"
	@echo ""
	@echo "Variables:"
	@echo "  CXX, STD, OPT, WARN, DEBUG, EMBED, PGOEMBED, EXTRA_INC, EXTRA_LIB, EXTRA_LLVM_LIBS"
	@echo "  LLVM_CONFIG=$(LLVM_CONFIG)  HAVE_LLVM=$(HAVE_LLVM)"

env:
	@echo "OS:          $(UNAME_S)"
	@echo "CXX:         $(CXX)"
	@echo "EMBED:       $(EMBED)"
	@echo "PGOEMBED:    $(PGOEMBED)"
	@echo "LLVM_CONFIG: $(LLVM_CONFIG) (have=$(HAVE_LLVM))"
	@echo "CXXFLAGS:    $(CXXFLAGS)"
	@echo "LDFLAGS:     $(LDFLAGS)"

# --- Build the compiler -------------------------------------------------------
$(CSCRIPT_BIN): $(CSCRIPT_CPP)
	$(CXX) $(CXXFLAGS) $(DEFS) $< -o $@ $(LDFLAGS)

# --- Pattern rule: build a .csc file into a single .exe using cscriptc --------
# Example: make foo.exe   (requires foo.csc in current dir)
%.exe: %.csc $(CSCRIPT_BIN)
	./$(CSCRIPT_BIN) $< -o $@

# --- Convenience: run-<name> will build <name>.exe and run it -----------------
run-%: %.csc $(CSCRIPT_BIN)
	./$(CSCRIPT_BIN) $< -o $*.exe && ./$(basename $@).exe

# --- Examples folder ----------------------------------------------------------
examples: $(EX_BINS)

run-examples: examples
	@set -e; \
	for b in $(EX_BINS); do \
	  echo "==> ./$$b"; \
	  ./$$b || exit $$?; \
	done

# --- Install / Uninstall ------------------------------------------------------
install: $(CSCRIPT_BIN)
	install -d "$(BINDIR)"
	install -m 0755 "$(CSCRIPT_BIN)" "$(BINDIR)/$(CSCRIPT_BIN)"
	@echo "Installed $(CSCRIPT_BIN) -> $(BINDIR)"

uninstall:
	rm -f "$(BINDIR)/$(CSCRIPT_BIN)"
	@echo "Removed $(BINDIR)/$(CSCRIPT_BIN)"

# --- Clean -------------------------------------------------------------------
clean:
	rm -f $(CSCRIPT_BIN) *.o *.obj *.exe
	@if [ -d "$(EXDIR)" ]; then rm -f $(EX_BINS); fi

