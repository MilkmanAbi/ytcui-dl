# ytcui-dl Makefile
# Targets: Linux, macOS, FreeBSD, OpenBSD, NetBSD

UNAME_S := $(shell uname -s)

ifeq ($(origin CXX),default)
    ifeq ($(UNAME_S),Darwin)
        CXX := clang++
    else ifeq ($(UNAME_S),FreeBSD)
        CXX := clang++
    else ifeq ($(UNAME_S),OpenBSD)
        CXX := clang++
    else ifeq ($(UNAME_S),NetBSD)
        CXX := clang++
    else
        CXX := g++
    endif
endif

CXXFLAGS = -std=c++17 -O3 -Wall -Wextra -Iinclude

# Platform-specific flags
ifeq ($(UNAME_S),Darwin)
    # Homebrew curl/openssl on macOS
    BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
    CURL_PREFIX := $(BREW_PREFIX)/opt/curl
    SSL_PREFIX  := $(BREW_PREFIX)/opt/openssl@3
    ifneq ($(wildcard $(CURL_PREFIX)/lib/libcurl.*),)
        CXXFLAGS  += -I$(CURL_PREFIX)/include -I$(SSL_PREFIX)/include
        LDFLAGS    = -L$(CURL_PREFIX)/lib -L$(SSL_PREFIX)/lib \
                     -lcurl -lssl -lcrypto -lpthread
    else
        LDFLAGS    = -lcurl -lssl -lcrypto -lpthread
    endif
    CXXFLAGS  += -DYTFAST_MACOS
else ifeq ($(UNAME_S),FreeBSD)
    CXXFLAGS  += -I/usr/local/include -DYTFAST_BSD
    LDFLAGS    = -L/usr/local/lib -lcurl -lssl -lcrypto -lpthread
else ifeq ($(UNAME_S),OpenBSD)
    CXXFLAGS  += -I/usr/local/include -DYTFAST_BSD
    LDFLAGS    = -L/usr/local/lib -lcurl -lssl -lcrypto -lpthread
else ifeq ($(UNAME_S),NetBSD)
    CXXFLAGS  += -I/usr/pkg/include -DYTFAST_BSD
    LDFLAGS    = -L/usr/pkg/lib -lcurl -lssl -lcrypto -lpthread
else
    # Linux
    LDFLAGS    = -lcurl -lssl -lcrypto -lpthread
    CXXFLAGS  += -DYTFAST_LINUX
endif

BIN = ytcui-dl
CLI_SRC = cli/ytcui-dl.cpp
TEST_SRC = test/test_ytfast.cpp

.PHONY: all clean install uninstall test

all: $(BIN)

$(BIN): $(CLI_SRC) include/ytfast.h include/ytfast_innertube.h include/ytfast_http.h include/ytfast_types.h
	$(CXX) $(CXXFLAGS) -o $@ $(CLI_SRC) $(LDFLAGS)
	@echo "Built: $(BIN)"

test/test_ytfast: $(TEST_SRC) include/ytfast.h
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_SRC) $(LDFLAGS)

test: test/test_ytfast
	./test/test_ytfast

clean:
	rm -f $(BIN) test/test_ytfast

PREFIX ?= /usr/local
install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	@echo "Installed to $(PREFIX)/bin/$(BIN)"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
