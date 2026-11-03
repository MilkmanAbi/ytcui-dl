# One executable, including browser sessions and HLS quality selection.
PREFIX ?= /usr/local
JOBS ?= 2
.PHONY: all test install clean
all:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
	cmake --build build --parallel $(JOBS)
	cp build/ytcui-dl ./ytcui-dl
test: all
	ctest --test-dir build --output-on-failure
install: all
	cmake --install build --prefix "$(PREFIX)"
clean:
	cmake --build build --target clean
	rm -f ./ytcui-dl
