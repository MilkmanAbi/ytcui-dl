# ytcui-dl 0.6.1

[![CI](https://github.com/MilkmanAbi/ytcui-dl/actions/workflows/ci.yml/badge.svg)](https://github.com/MilkmanAbi/ytcui-dl/actions/workflows/ci.yml)

One C++17 executable for custom YouTube extraction, independent audio/video format
selection, high-resolution downloads, live HLS playback and browser sessions.
The live HLS and session-helper work is integrated directly into main.

## Downloads and build

[Release archives and SHA-256 checksums](https://github.com/MilkmanAbi/ytcui-dl/releases)
cover Linux x86_64/ARM64, Alpine musl x86_64, macOS Intel/Apple Silicon, and
FreeBSD/OpenBSD/NetBSD x86_64. Install your platform's OpenSSL and zlib runtime
libraries. macOS uses Homebrew openssl@3 and zlib. Archives are OS/architecture
specific; Linux glibc and musl builds are separate.

The custom transport and session code use POSIX APIs. On Windows use WSL;
`ytcui-dl.ps1` forwards arguments to a source build in WSL. This project does not
provide a native Windows executable.

```sh
# Debian/Ubuntu build dependencies
sudo apt-get install build-essential cmake ninja-build libssl-dev zlib1g-dev python3
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$HOME/.local"
# make, make test and make install PREFIX=... are also supported
```

Python is used for offline tests and packaging; it is not a runtime dependency.
Use `-DBUILD_TESTING=OFF` for an application-only build. Install mpv for playback
and ffmpeg for muxing/remuxing. Neither tool is bundled. The application does not
run Python, yt-dlp or an embedded browser.

## Resolution, playback and downloads

```sh
ytcui-dl -F VIDEO_URL                      # all direct formats
ytcui-dl -L VIDEO_URL                      # resolution/audio ladder
ytcui-dl -p -q 1080 VIDEO_URL              # target 1080p with independent audio
ytcui-dl -p -q best VIDEO_URL              # highest available resolution
ytcui-dl -p --max-height 720 VIDEO_URL     # hard height ceiling
ytcui-dl -d -q 2160 -o video.mkv VIDEO_URL # high-resolution download, then mux
ytcui-dl -a -g VIDEO_URL                   # selected audio URL
ytcui-dl -j VIDEO_URL                      # metadata JSON
ytcui-dl --mpv-args -q 1080 VIDEO_URL      # print playback arguments
```

`-q HEIGHT` chooses the best at or below the target. If every available stream is
higher, it chooses the nearest higher stream. `--max-height` is a hard ceiling
and fails when no candidate fits. Playback also considers local decoder support;
`--any-codec` allows a stream regardless of that check. High-resolution video and
audio are selected separately; a low-resolution muxed stream does not impose a
360p ceiling on adaptive video. Quality is limited by the formats the selected
YouTube client actually returns, not a promise that every video has 4K.

Direct downloads validate HTTP ranges and byte counts. Partial output is stored
as `.part` with a `.part.ranges` ledger of durable completed chunks. Resume uses
that ledger instead of trusting preallocated file size. Only successful transfers
create the final filename; an existing final output is not overwritten. Changing
the signed source URL or chunk layout invalidates the ledger safely. Servers that
ignore ranges use a fresh sequential transfer. `--no-resume` refetches all chunks.

## Live and HLS

```sh
ytcui-dl -L LIVE_URL
ytcui-dl -p -q 720 LIVE_URL
ytcui-dl -p --hls --max-height 1080 VIDEO_URL
ytcui-dl -g --hls -q 720 VIDEO_URL
```

The client parses master playlists and selects the requested variant itself,
including its external audio group. mpv receives the chosen video/audio URLs;
it does not independently pick an arbitrary resolution. `-F` and `-L` show live
HLS variant heights, frame rates and bandwidth. A media playlist without master
resolution metadata cannot satisfy an explicit height request.

Live streams use HLS. `--hls` selects HLS for VOD playback/URL output when offered;
`--no-hls` disables automatic VOD fallback. Direct `-d` operates on byte resources,
so HLS recording/download is a separate ffmpeg operation. In audio+video mode
`-g --hls` can print two URLs when the selected variant uses external audio.

## Integrated browser sessions

```sh
ytcui-dl session --help
ytcui-dl session login --print-command    # inspect the browser launch command
ytcui-dl session login                    # open your installed browser
ytcui-dl session list
ytcui-dl session export --browser firefox
ytcui-dl session check
ytcui-dl session play VIDEO_URL -q 1080
# explicit database and destination:
ytcui-dl session export --browser firefox --profile /path/to/cookies.sqlite -o cookies.txt
ytcui-dl --cookies cookies.txt -p VIDEO_URL
```

Only explicit session commands inspect browser data. Ordinary media commands use
an explicit `--cookies` file; they do not silently read a browser profile. Login
opens the installed browser so it handles passwords and authentication. Export
reads supported Firefox/Chromium-family cookie stores into a private 0600 Netscape
jar, defaulting to `$XDG_CONFIG_HOME/ytcui-dl/cookies.txt` or
`~/.config/ytcui-dl/cookies.txt`. Session playback attaches that jar automatically.

Chromium decryption uses the platform's Safe Storage/keyring integration where
supported. Locked or unsupported stores are reported. Close the browser before
exporting: the small read-only SQLite parser does not replay uncheckpointed WAL
records. Safari cookie stores and native Windows Chromium app-bound encryption
are unsupported. CI uses synthetic databases and never accesses real profiles.

A session can enable content your account may access, but it does not generate
GVS proof-of-origin tokens. Some clients/videos remain restricted or byte capped.
`--po-token`, `--pot-provider` and `--pot-provider-url` support externally supplied
tokens. This custom client does not execute YouTube player JavaScript or implement
all behavior of yt-dlp. Service changes can break extraction despite passing CI.

## Audit and CI/CD

[AUDIT.md](AUDIT.md) records the audit's fixes and retained limits. CI builds with
GCC/Clang and hardened libc++, Linux x86_64/ARM64, macOS 15/26 Intel/Apple Silicon,
Alpine, FreeBSD, OpenBSD and NetBSD. It runs parser/SQLite/selection regressions,
synthetic session exports, deterministic HTTP download/resume/redirect tests,
ASan/UBSan, Make, installation and unpacked-archive checks. Tests run offline.

Pushing a `vVERSION` tag runs the complete matrix and publishes archives only
when every check passes and the tag matches VERSION. Workflows never commit
source changes. The original MIT license and contributor history are preserved.
