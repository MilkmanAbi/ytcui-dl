# ytcui-dl Makefile
# Targets: Linux, macOS, FreeBSD, OpenBSD, NetBSD
#
# Dependencies: OpenSSL (libssl, libcrypto), zlib, pthreads.
# libcurl is no longer required — see include/yt_http.h.

UNAME_S := $(shell uname -s)

ifeq ($(origin CXX),default)
    ifneq (,$(filter $(UNAME_S),Darwin FreeBSD OpenBSD NetBSD))
        CXX := clang++
    else
        CXX := g++
    endif
endif

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

ifeq ($(UNAME_S),Darwin)
    BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
    SSL_PREFIX  := $(BREW_PREFIX)/opt/openssl@3
    ifneq ($(wildcard $(SSL_PREFIX)/lib/libssl.*),)
        CXXFLAGS += -I$(SSL_PREFIX)/include
        LDFLAGS  := -L$(SSL_PREFIX)/lib $(LDFLAGS)
    endif
    LDFLAGS += -Wl,-dead_strip
    CXXFLAGS += -DYTFAST_MACOS
else ifneq (,$(filter $(UNAME_S),FreeBSD OpenBSD NetBSD))
    CXXFLAGS += -I/usr/local/include -DYTFAST_BSD
    LDFLAGS  := -L/usr/local/lib $(LDFLAGS) -Wl,--gc-sections
else
    LDFLAGS += -Wl,--gc-sections -Wl,-O1 -Wl,--as-needed
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
