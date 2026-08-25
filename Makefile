# ytcui-dl Makefile
# Targets: Linux, macOS, FreeBSD, OpenBSD, NetBSD
#
# Dependencies: OpenSSL (libssl, libcrypto), zlib, pthreads.
# libcurl is no longer required — see include/yt_http.h.

UNAME_S := $(shell uname -s)

# No CXX override here on purpose. `c++` (GNU Make's own built-in default,
# confirmed via `make -p`) is already correct everywhere that matters: it's
# each platform's own symlink to its actual default compiler -- Apple's
# toolchain on macOS (straight to clang++, so forcing `clang++` explicitly
# was a no-op there), and real GCC on NetBSD, whose base system doesn't ship
# clang by default -- so that same override was actively wrong there,
# silently trying to build with a compiler that isn't installed.
# `make CXX=whatever` still works for anyone who wants something specific;
# this file just stops guessing and lets each OS's own symlink decide.

VERSION  := $(shell cat VERSION 2>/dev/null || echo 0.4.0)
PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin

# -Os beats -O3 here: the hot paths are memory-bound scanning, so a smaller
# text section wins more from instruction cache than unrolling wins from ILP.
CXXFLAGS ?= -Os
CXXFLAGS += -std=c++17 -Wall -Wextra -Iinclude \
            -ffunction-sections -fdata-sections \
            -fno-plt -fvisibility=hidden -fvisibility-inlines-hidden \
            -fno-asynchronous-unwind-tables -fno-unwind-tables \
            -DYTFAST_VERSION=\"$(VERSION)\"

LDFLAGS  += -lssl -lcrypto -lz -lpthread

# Where to find OpenSSL's headers/libs, when they're not already on the
# compiler's default search path. Set this instead of patching the Makefile
# -- `make OPENSSL_PREFIX=/path/to/openssl` -- for a non-Homebrew macOS
# install, a custom-built OpenSSL, a pkgsrc/ports prefix that isn't
# /usr/local, or anything else this can't guess. Left unset, it falls back
# to `brew --prefix openssl@3` on macOS (only if Homebrew is actually
# installed and that keg exists) and to /usr/local on the BSDs, which is
# where their package managers put it by default; Linux distros typically
# ship OpenSSL where the compiler already looks, so nothing extra happens
# there unless OPENSSL_PREFIX is set. (Left genuinely unset rather than
# defaulted to empty here on purpose: `?=` further down only fires for a
# variable that has never been assigned at all, so this can't pre-empt it.)

ifeq ($(UNAME_S),Darwin)
    ifeq ($(OPENSSL_PREFIX),)
        BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
        ifneq ($(BREW_PREFIX),)
            ifneq ($(wildcard $(BREW_PREFIX)/opt/openssl@3/lib/libssl.*),)
                OPENSSL_PREFIX := $(BREW_PREFIX)/opt/openssl@3
            endif
        endif
    endif
    LDFLAGS  += -Wl,-dead_strip
    CXXFLAGS += -DYTFAST_MACOS
else ifneq (,$(filter $(UNAME_S),FreeBSD OpenBSD NetBSD))
    OPENSSL_PREFIX ?= /usr/local
    CXXFLAGS += -DYTFAST_BSD
    LDFLAGS  := $(LDFLAGS) -Wl,--gc-sections
else
    LDFLAGS += -Wl,--gc-sections -Wl,-O1 -Wl,--as-needed
endif

ifneq ($(OPENSSL_PREFIX),)
    CXXFLAGS += -I$(OPENSSL_PREFIX)/include
    LDFLAGS  := -L$(OPENSSL_PREFIX)/lib $(LDFLAGS)
endif

# C++ exceptions are used for transport errors, so -fno-exceptions is out;
# RTTI is not used anywhere and costs typeinfo in .rodata.
CXXFLAGS += -fno-rtti

BIN     := ytcui-dl
SRC     := cli/ytcui-dl.cpp
HEADERS := $(wildcard include/*.h) $(wildcard cli/*.h)

.PHONY: all clean install uninstall test test-live diag size

all: $(BIN)

$(BIN): $(SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(LDFLAGS)
	@strip $@ 2>/dev/null || true

# Offline: parser, extraction, itag table, selection engine. No network.
test: test/test_yj test/test_client test/test_select
	./test/test_yj test/fixtures
	./test/test_client test/fixtures
	./test/test_select test/fixtures

# Network: TLS, gzip, redirects, live InnerTube, download engine.
test-live: test/test_http test/test_live test/test_download
	./test/test_http
	./test/test_live
	./test/test_download

# What is broken, and where.
diag: $(BIN)
	./$(BIN) --diag

test/test_%: test/test_%.cpp $(HEADERS)
	$(CXX) -std=c++17 -O2 -Wno-deprecated-declarations -Iinclude $(filter -I%,$(CXXFLAGS)) -o $@ $< $(LDFLAGS)

size: $(BIN)
	@echo "binary:  $$(stat -c%s $(BIN) 2>/dev/null || stat -f%z $(BIN)) bytes"
	@echo "deps:    $$(ldd $(BIN) 2>/dev/null | wc -l) shared objects"
	@size $(BIN) 2>/dev/null || true

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)

clean:
	rm -f $(BIN) test/test_yj test/test_client test/test_select test/test_http test/test_live test/test_download
