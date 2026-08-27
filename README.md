# ytcui-dl

> **AI Disclosure:** ytcui-dl v1 was written entirely by hand. v2 is a full refactor developed over several months, with help from a friend. AI was used for debugging, testing functionality, and automating the final push to the repository. The project architecture and implementation are my own, while AI assisted with resolving issues during development.

> PO-Token support has landed: `--pot-provider` delegates to a locally-run
> [bgutil-ytdlp-pot-provider](https://github.com/Brainicism/bgutil-ytdlp-pot-provider)
> for the JS-challenge method (the same one yt-dlp itself defers to -- nothing
> mints a token without running Google's JS, yt-dlp's own core included), and
> `--cookies` adds real logged-in-session authentication. Details in
> [Known constraints](#known-constraints).

A small, header-only YouTube client in C++17. Resolves streams, plays them,
downloads them. No libcurl, no JSON library, no yt-dlp.

```
binary        240 KB
shared objs   5        (libssl, libcrypto, libz, libSystem, libc++ — macOS; libstdc++/libgcc/libc/libm/vdso on Linux)
headers       ~4,500 lines total
build         ~4 s
cold start    ~70 ms to a resolved stream
```

## v2: a ground-up rewrite, not a patch

This is a full rewrite of the original `ytcui-dl`, not an incremental update
to it. The old version worked, in the sense that it ran — but it had a
ceiling baked into its architecture that no amount of bug-fixing was going to
lift. Concretely, diffed against the old codebase directly:

| | v1 (old) | v2 (this repo) |
|---|---|---|
| Dependencies | libcurl + bundled nlohmann/json (a multi-thousand-line single header) | none beyond OpenSSL/zlib/pthreads — custom HTTP/1.1 client (`yt_http.h`) and a zero-allocation JSON scanner (`yj.h`) |
| Best download, ever | **Hard-capped at 360p.** `resolve_format_string("best", ...)` only ever picked a *muxed* stream, and itag 18 (360p, ~96 kbps AAC) is the only muxed adaptive format YouTube serves. No amount of asking for "best" got past it. | Video and audio are resolved and downloaded **independently**, then muxed with `ffmpeg` — the actual ceiling is whatever YouTube encoded (2160p60 for ordinary uploads, confirmed 4320p60 HDR / 8K on an 8K upload — see `ENCODING_AND_RESOLUTION.md`) |
| Download mechanism | One `curl_easy_perform()` call, one connection, no resume, no retry | Parallel chunked engine (default 3 connections, tunable), automatic resume, byte-identical output verified against a checksum test, and adaptive per-chunk retry/split tuned to this CDN's actual behavior (see `Downloader::fetch_span` in `yt_download.h`) |
| SABR (URL-less format lists) | **Not detected at all.** If InnerTube handed back adaptive formats with no `url` field, the old client had no way to know and would fail with formats that looked fine but weren't fetchable | An explicit SABR gate (`sabr_only`) rejects any response whose formats carry no URLs and advances to the next client in the chain instead of silently settling for garbage |
| Client chain | `ANDROID → IOS`, both hardcoded, no fallback logic for either SABR or PO-Token walls | `VISIONOS → ANDROID_VR → ANDROID → IOS`, chosen and actively re-tuned against yt-dlp's own current client policy table (see [Known constraints](#known-constraints) — this is a moving target and the README says exactly how to re-tune it) |
| User-Agent on media fetch | **Hardcoded to the ANDROID UA**, sent regardless of which client actually resolved the format URL — silently wrong the moment a fallback client won instead | Tracked per-video (`VideoInfo::client_ua`) from whichever client in the chain actually resolved it, and used consistently for every later request against that video (media fetch, `--play`, download) |
| Audio-only / video-only "just give me a file" | `-x` just meant "download bestaudio," raw, whatever container YouTube served — no transcoding | `-x`/`--remux` (`yt_stream_dl.h`) always produces one finished `.mp3` or `.mp4`, remuxing when the source codec is container-legal and transcoding automatically when it isn't |
| Diagnostics | None — a failure was just a stack of unexplained errors | `--diag` checks environment → network → every client in the chain → selection → an actual media fetch **near the end of the stream**, specifically because a fetch near the *start* passes even on a client that's about to hit a PO-Token wall |
| Non-ASCII titles/queries | Untested; filename generation had no explicit UTF-8 handling | Tested against Japanese, Korean, Cyrillic, Arabic, Thai, emoji, and ZWJ sequences; two real byte-vs-codepoint truncation bugs found and fixed (see `ENCODING_AND_RESOLUTION.md`) |
| Tests | None in the repo | `make test` (offline: parser/extraction/selection against captured fixtures, including a real SABR response) and `make test-live` (network: TLS, InnerTube, parallel/serial/resumed downloads verified byte-identical) |

The sections below are this rewrite's own documentation — architecture,
constraints, and the specific bugs fixed along the way, including ones that
predate this rewrite and were carried over from v1 until now.

## Why the old version only gave you 360p

itag 18 is a muxed 360p stream with a ~96 kbps AAC track. It was the only thing
the previous version could return, and the format selector was not the reason.

The InnerTube client version it used, `ANDROID 21.02.35`, receives a **SABR**
player response: the complete adaptive format list comes back with **no `url`
on any entry**, only `initRange`/`indexRange` and a top-level
`serverAbrStreamingUrl`. Fetching those needs YouTube's UMP/SABR protocol. The
single muxed itag 18 was the one directly fetchable stream left.

It failed silently in the worst way: `playabilityStatus` was `OK` and the
format list was non-empty, so nothing looked broken and no fallback fired.

Measured against a 4K60 upload:

```
before:   1 format,   360p,   ~96 kbps AAC
after:   29 formats,  2160p,  389 kbps 5.1
```

The client chain (currently `VISIONOS -> ANDROID_VR -> ANDROID -> IOS`, see
[Known constraints](#known-constraints) for why it leads with `VISIONOS`) has
a SABR gate that rejects any response whose adaptive formats carry no URLs, so
it advances to the next client instead of settling for muxed leftovers.

## Quick start

```sh
make
make diag          # what works, what doesn't, and why
```

```sh
ytcui-dl -a -g <url>                   # audio stream URL
ytcui-dl -p -q 720 <url>               # play at 720p
ytcui-dl -a -p <url>                   # play as audio only
ytcui-dl -d -q 1080 --vcodec avc1 <url>
ytcui-dl -a -d -o track.m4a <url>
ytcui-dl -x <url>                      # audio, straight to .mp3
ytcui-dl -d -q 1080 --remux <url>      # video, straight to .mp4
ytcui-dl -L <url>                      # what qualities exist
ytcui-dl -s "query" --json
```

Build dependencies: OpenSSL, zlib, pthreads. Optional at runtime: `mpv` for
`--play`, `ffmpeg` for muxing downloaded video+audio.

## Downloading, from the command line

Downloads went from "hangs or 403s on anything longer than about a minute"
to "full 4K in seconds" partway through this project's life — not a tuning
change, an actual wrong-client bug (the client chain was resolving through
one that needs a Proof-of-Origin token this project can't mint; see
[Known constraints](#known-constraints) for the specifics and why it may
need re-checking again someday). What's below is what it looks like now
that it works.

There are two ways to download, picked by which flag you reach for:

**`-d` / `--download`** — this project's own engine (`Downloader::fetch`,
`yt_download.h`): parallel connections, automatic resume, and it hands you
back whatever format YouTube actually served (raw `.m4a`/`.opus`/`.webm`
for audio-only, muxed into the container yt reports for video+audio via
`ffmpeg` if it's on `$PATH`). Reach for this when you want a specific
resolution/codec/itag, or you're downloading something big enough that
resume actually matters.

**`-x` / `--extract-audio`** and **`--remux`** — an opt-in second path
(`StreamDownloader::fetch`, `yt_stream_dl.h`) for when you just want a
finished, ordinary file and don't care about the mechanics: `-x` always
produces a `.mp3` (transcoded, since YouTube never serves MP3 source);
`--remux` (combine with `-d`) always produces a `.mp4` (remuxed with no
re-encode when the source codec allows it, transcoded automatically the
rare times it doesn't). Under the hood it still uses this project's own
fetch engine to actually pull the bytes — handing the raw URL straight to
`ffmpeg` and letting it fetch measures out to roughly realtime speed on this
CDN, not full bandwidth, for reasons in the "Streaming through ffmpeg"
section further down — so `ffmpeg` only ever touches a local temp file, not
the network.

```sh
# -d: this project's own engine, keeps the source format(s)
ytcui-dl -d <url>                          # best video+audio, muxed via ffmpeg
ytcui-dl -a -d <url>                       # audio only, raw (.m4a/.opus/...)
ytcui-dl -V -d -q 1080 <url>               # video only, capped at 1080p
ytcui-dl -d -q 1080 --vcodec avc1 <url>    # 1080p, H.264 specifically
ytcui-dl -d --itag 251 <url>               # one exact format, no selection logic
ytcui-dl -d -c 1 <url>                     # single connection (default is 3)
ytcui-dl -d -o track.m4a <url>             # explicit output path
ytcui-dl -d <url>                          # re-run on a partial file: resumes

# -x / --remux: ffmpeg finishes the job, one file, no raw leftovers
ytcui-dl -x <url>                          # audio -> .mp3
ytcui-dl -d --remux <url>                  # video+audio -> .mp4
ytcui-dl -V -d --remux -q 720 <url>        # video only -> .mp4, capped at 720p
```

Quality flags (`-q`/`--quality`, `--max-height`, `--vcodec`, `--acodec`,
`--container`, `--hdr`, `--stereo`, `--itag`, ...) work identically for both
paths — they decide *what* gets fetched; `-d` vs `-x`/`--remux` only decides
*what happens to it afterward*. Full flag list: `ytcui-dl --help`.

## Three modes

```cpp
Mode::AudioOnly    // audio track only        — music, radio, background
Mode::VideoOnly    // video track, no audio   — silent preview, scrubbing
Mode::AudioVideo   // both                    — normal playback
```

Video and audio are selected **independently**. That is what lifts quality
above the muxed ceiling, and it is what makes resolution switching cheap.

## Library use

Header-only. `#include "ytfast.h"`, link `-lssl -lcrypto -lz -lpthread`.

```cpp
#include "ytfast.h"
using namespace ytfast;

CurlGlobalInit init;                       // once, at startup
auto& yt = InnertubeClient::get_instance();

auto r = yt.resolve("aqz-KE-bpKQ", Mode::AudioVideo, Quality::at(1080));
if (r.ok()) {
    r.sel.video->url;        // video-only stream
    r.sel.audio->url;        // audio-only stream
    r.sel.describe();        // "1080p60 av01 + 387kbps mp4a 6ch"
    r.ladder;                // every rung, for a quality menu
}

yt.shutdown();                             // joins the prefetch worker
```

### Quality

Every field is optional. A default-constructed `Quality` asks for the best
available and always resolves to something.

```cpp
Quality q;
q.height         = 1080;     // target; 0 = best
q.max_height     = 1440;     // hard ceiling
q.fps            = 60;       // prefer at least this
q.prefer_hdr     = true;
q.allow_surround = false;    // stereo only
q.allow_muxed    = false;    // forbid the muxed fallback
q.smallest       = true;     // invert everything: smallest picture, lowest bitrate
q.vcodec         = "av01";   // av01 | vp9 | avc1 | h264
q.acodec         = "opus";   // opus | mp4a | aac
q.container      = "webm";   // mp4 | webm

Quality::best();  Quality::at(720);  Quality::upto(1080);  Quality::lowest();
```

Constraints relax in tiers, so an over-specified request degrades to a working
pick rather than returning nothing. Asking for a height that does not exist
lands on the **nearest rung above** it, not on the largest stream available —
requesting 240p on a 1080p-and-2160p upload gives you 1080p.

### Graceful resolution switching

In `AudioVideo` mode the audio track is chosen independently of the video, so
changing resolution only swaps the video stream. A player can reload the video
input without a gap in sound and without re-buffering audio.

```cpp
auto sel  = Selector::select(info.formats, Mode::AudioVideo, Quality::at(1080));
auto next = Selector::switch_video(info.formats, sel, 720);
// next.audio == sel.audio   — guaranteed, and covered by a test
```

`switch_video` returns the current selection unchanged when the video track
cannot change, so a caller can compare pointers to decide whether a reload is
even needed.

```cpp
int h = Selector::step_height(info.formats, current_height, +1);   // one rung up
```

Stepping settles at either end rather than wrapping.

### Quality menu

```cpp
for (const auto& r : Selector::ladder(info.formats))
    printf("%-10s itag %d  %s\n", r.label().c_str(), r.itag,
           human_bytes(r.bytes).c_str());
```

One rung per height, with 60 fps kept separate from 30 fps because users do
choose between "720p" and "720p60". Sub-30 fps variants collapse into their
height's rung — a menu should not show "144p" twice.

`Selector::audio_ladder()` is the audio equivalent.

### Downloading

```cpp
DownloadOptions o;
o.connections = 3;                       // parallel ranged chunks; 1 disables
o.resume      = true;
o.on_progress = [](const Progress& p) {
    printf("\r%3.0f%%  %s/s  ETA %s", p.percent(),
           human_bytes((int64_t)p.speed_bps).c_str(),
           human_duration(p.eta_seconds).c_str());
    return true;                         // return false to cancel
};

auto r = Downloader::fetch(url, path, o);
```

Parallelism is not a micro-optimisation. YouTube rate-limits an individual
connection well below what a link can carry, so a single-stream download is
throttle-bound rather than bandwidth-bound. Measured 2.84x on a 4.4 MB file
with byte-identical output.

Workers `pwrite()` into their own regions of one preallocated file, so there is
no locking and no shared file offset. A server that ignores `Range`, or a
response with no `Content-Length`, falls back to a single sequential stream.

Resume opens `r+b`. Opening `wb` truncates, which leaves a hole of zeros at the
head of the file after seeking to the resume offset — that bug was in the
previous version and is now covered by a checksum test.

### Streaming through ffmpeg (`-x` / `--remux`)

```sh
ytcui-dl -x <url>                    # audio -> .mp3
ytcui-dl -d --remux <url>            # video+audio -> .mp4
ytcui-dl -V -d --remux <url>         # video only -> .mp4, no audio track
```

```cpp
auto r = StreamDownloader::fetch(video_url, audio_url, out_path,
                                 info.client_ua, /*audio_only=*/false);
r.transcoded;  // false = remuxed (stream copy, no re-encode)
```

`-d`/`Downloader::fetch` leaves you with the exact format(s) YouTube served —
useful when you want a specific itag or to keep video and audio as separate
files. `-x`/`--remux`/`StreamDownloader::fetch` is for when the goal is
"give me a working `.mp3`" or "give me a working `.mp4`" and the exact
mechanics don't matter: it always produces one finished file, transcoding
audio to MP3 and remuxing (or, only if that fails, transcoding) video+audio
into MP4.

The obvious implementation hands ffmpeg the googlevideo URL directly and lets
its own HTTP client fetch it. That measures out badly: ffmpeg's http protocol
holds one continuous connection for the whole input, and past an initial
burst this CDN throttles a sustained read on one connection to roughly
realtime speed rather than full bandwidth — a 634-second video came down at
~1.5x through ffmpeg directly, versus ~180x through this project's own
chunked engine on the same file, which never holds one connection open long
enough to trigger the throttle (see `Downloader::fetch_span` and the
`VISIONOS` PO-Token note in [Known constraints](#known-constraints) — same
underlying CDN behavior, different symptom). So `StreamDownloader::fetch`
pulls the source stream(s) to a temp file through `Downloader::fetch` first —
fast, and already proven against this exact throttle — and only ever hands
ffmpeg local paths, where none of it applies.

### Playing

The CLI can print the exact mpv invocation instead of running it:

```sh
$ ytcui-dl -b --mpv-args -q 720 <url>
mpv --ytdl=no --user-agent=<...> --force-window=yes <video-url> --audio-file=<audio-url>

$ ytcui-dl -a --mpv-args <url>
mpv --ytdl=no --user-agent=<...> --no-video <audio-url>
```

The user agent must match the client that signed the URL or the CDN rejects it.

## Playback vs downloading

Ranking by compression is right for downloading and wrong for playing. AV1
compresses best, so a compression-ranked selector reaches for it every time --
but AV1 hardware decode only exists on recent GPUs, and software AV1 above
1080p stalls or kills a player on most machines. A stream that will not decode
is worse than a larger one that will.

`Quality::playback()` ranks by what the machine can actually do. `--caps` shows
what was detected:

```
$ ytcui-dl --caps
decoders: h264(hw) hevc(hw) vp9(hw) av1(hw)
```

Probed once from `mpv --vd=help` and `mpv --hwdec=help`, then cached for a
week. Codecs with no decoder are never selected; software-only codecs are
ranked down above 1080p. With mpv absent it assumes H.264 and VP9 rather than
refusing to play anything.

```
-q 2160            2160p60 av01   (smallest file)
-q 2160 --playable 2160p60 vp9    (cheapest to decode here)
```

### Graceful degradation

`Selector::degrade()` returns the next thing to try after playback failed:
same picture with a cheaper codec first, then a rung down, then the muxed
stream. It returns an unusable selection when exhausted, so a retry loop
terminates.

```
2160p60 vp9  ->  1440p60 vp9  ->  1080p60 avc1  ->  720p60 avc1
             ->  480p avc1    ->  ...           ->  360p avc1 (muxed)
```

A player that dies seconds in has told you the pick is unusable, and the format
list cannot say why -- missing decoder, a driver claiming hardware support it
lacks, a link that cannot sustain the bitrate. Stepping down is the only honest
response.

## yt-dlp compatible mode

Symlink it and existing scripts work:

```sh
ln -s ytcui-dl yt-dlp
yt-dlp -f 'bestvideo[height<=1080]+bestaudio' -o '%(title)s.%(ext)s' <url>
yt-dlp -x --audio-format mp3 <url>
yt-dlp -F <url>
yt-dlp -g -f bestaudio <url>
yt-dlp --print '%(id)s %(resolution)s' <url>
```

Also reachable as `ytcui-dl --yt-dlp ...`.

A separate mode rather than one merged flag set, because the interfaces
genuinely collide: `-q` is `--quality` here and `--quiet` there, `-a` is
`--audio` here and `--batch-file` there, `-c` is `--connections` vs
`--continue`, `-s` is `--search` vs `--simulate`. Merging them would silently
do the wrong thing for one set of users.

Supported: `-f` (including `+` pairs, `/` fallback chains, and
`[height<=N][ext=][fps>=][vcodec=][tbr<=]` filters), `-o` templates, `-P`,
`-F`, `-j`, `-g`, `-e`, `--get-*`, `--print`, `-x`, `--audio-format`,
`--merge-output-format`, `-N`, `-c`/`--no-continue`, `-w`, `-s`, `-4`/`-6`,
`--user-agent`. Unknown options are accepted and ignored so existing command
lines still run.

Not implemented: playlists and channels, subtitles, cookies, SponsorBlock,
archives, authentication.

## Embedded notes

- **Header-only**, no build system requirements beyond a C++17 compiler.
- **No global state is required.** `InnertubeClient` is constructible directly;
  the singleton is a convenience, not a dependency.
- **Selection does no I/O and no allocation.** `Selector` operates on an
  already-fetched format list, so it is safe to call from a UI thread on every
  keystroke.
- **One background thread, at most.** The prefetch worker starts on first use
  and is joined by `shutdown()`. Never call it and there is no thread at all.
- **The disk cache is optional**: `set_disk_cache(false)` for read-only
  filesystems. It only ever holds `visitorData`.
- **`StreamFormat` holds one `std::string`** (the URL). Everything else is a
  `string_view` into a static itag table or the owning `VideoInfo`'s arena, so
  a `StreamFormat` must not outlive its `VideoInfo`.
- **Exceptions** are used for transport errors only. Parsing never throws:
  malformed JSON yields empty values, never UB.

## Diagnostics

`--diag` checks each layer in order, so the first failure identifies where the
problem actually is:

```
environment      openssl, cache dir, ffmpeg, mpv
network          HTTPS reachability, visitorData
innertube        every client in the chain, and whether it returned SABR
selection        all three modes plus the ladder
media fetch      actually pulls bytes from the CDN
```

The last check is the one that matters. Everything above it can pass while
playback still fails, because the CDN binds each signed URL to the IP that
requested it. A 403 there with everything else green means the URL was signed
for a different address than the one fetching it — a VPN or proxy egressing
elsewhere, or a dual-stack host where the two requests took different paths.
`-4` / `-6` force one address family and are the first thing to try.

## Testing

```sh
make test        # offline: parser, extraction, itag table, selection
make test-live   # network: TLS, gzip, redirects, InnerTube, downloads
```

Offline tests run against captured fixtures, including a real SABR response so
that regression cannot return quietly. Clean under ASan, UBSan, TSan, and
`-Wall -Wextra -Wpedantic -Wshadow`.

## Layout

```
include/
  yj.h            zero-allocation JSON scanner      (replaces nlohmann)
  yjw.h           JSON writer
  yt_itag.h       static itag table
  yt_types.h      StreamFormat, VideoInfo
  yt_http.h       HTTP/1.1 over OpenSSL             (replaces libcurl)
  yt_cache.h      on-disk visitorData cache
  yt_select.h     modes, ladder, switching
  yt_download.h   parallel downloads, muxing
  yt_stream_dl.h  -x/--remux: fetch + ffmpeg, one finished .mp3/.mp4
  yt_innertube.h  InnerTube client
  ytfast.h        umbrella header
```

### Parser

`yj.h` is a single-pass scanner: no DOM, no allocation, no recursion, no
exceptions. Strings are returned as `string_view` slices into the response
buffer; only fields you keep are copied. Verified byte-identical to nlohmann.

```
player 180 KB   1711 us / 4215 allocs / 463 KB  ->    98 us / 89 allocs / 54 KB
search 317 KB   4469 us / 22363 allocs / 1157 KB -> 1333 us / 60 allocs / 8.4 KB
```

`X-Goog-FieldMask` on every request cuts player responses from ~183 KB to
~69 KB with an identical format list.

## Known constraints

- **A version pin is load-bearing.** `ANDROID` is pinned to 20.10.38; newer
  versions get SABR. The tradeoff is that it returns `LOGIN_REQUIRED` without
  `visitorData`, so the bootstrap is not optional.
- **PO Tokens are not minted, on purpose -- nothing does this without running
  Google's JS.** A PO Token is a BotGuard attestation; producing one requires
  actually executing Google's obfuscated challenge JS somewhere, and that's
  true of yt-dlp too -- its own core repo ships zero token-minting logic, only
  a plugin interface, confirmed by reading it. Three ways to get one into
  ytcui-dl, in the order they matter:
  - **`--po-token <tok>`** -- paste one you already have (a browser extension,
    a manual mint, whatever). Used as-is.
  - **`--pot-provider`** (or `--pot-provider-url <url>` for a non-default
    address) -- fetches one automatically from a locally-running
    [bgutil-ytdlp-pot-provider](https://github.com/Brainicism/bgutil-ytdlp-pot-provider),
    the same external Node/Deno service yt-dlp's own `bgutil` plugin talks to
    (`POST /get_pot {content_binding}` -> `{poToken}`). This project doesn't
    solve the BotGuard challenge any more than yt-dlp's core does; it just
    speaks that provider's HTTP API. Run the provider yourself
    (`docker run brainicism/bgutil-ytdlp-pot-provider` is the easy path) and
    the flag does the rest. A response that doesn't decode as plausible
    base64url is rejected rather than sent on -- confirmed live that YouTube's
    player endpoint 400s the *entire* request (every client in the chain, not
    just the field) on a malformed token, so a broken or misconfigured
    provider degrades to "no token" instead of taking down playback.
  - **`--cookies <file>`** -- a Netscape-format `cookies.txt` (curl/yt-dlp
    convention). This is *not* a PO Token -- cookies alone don't produce one,
    confirmed against yt-dlp's own architecture -- but a `SAPISID` cookie gets
    turned into a real `SAPISIDHASH` `Authorization` header, the same
    origin-bound signature a logged-in browser sends, which does unlock
    age-gated/members-only/private videos and a logged-in `WEB` session for
    search/bootstrap.
- **`ANDROID_VR`/`ANDROID`/`IOS` now all require one for GVS media fetch, and
  the chain leads with `VISIONOS` (Apple Vision Pro) instead, which as of
  yt-dlp's current `INNERTUBE_CLIENTS` table still doesn't.** Those three still
  hand back a fetchable `url` per adaptive format -- the player response looks
  fine -- but as of 2026-08 the CDN caps how many bytes it will actually serve
  from that URL to roughly a minute of the stream (scaled by bitrate: ~3 MB
  for a 390 kbps audio track, ~10-12 MB for 1080p60 video) before 403ing
  everything past it, no matter the offset, request size, connection count, or
  how long you wait -- confirmed against fresh URLs, fresh connections, and
  unrelated videos, so it isn't this project's signing, IP, or retry logic.
  yt-dlp's own extractor dates the `ANDROID_VR` break to 2026-08-17. `VISIONOS`
  URLs have no such cap (confirmed pulling a 679 MB 2160p60 stream through one
  end to end); the tradeoff is it doesn't offer the 5.1 surround audio tracks
  (`mp4a`/`ec-3`/`ac-3` at ~387 kbps) those three do, topping out around opus
  ~155 kbps stereo instead. `ANDROID_VR`/`ANDROID`/`IOS` stay in the chain as
  fallbacks for what `VISIONOS` can't serve ("made for kids" videos) or where
  the cap doesn't matter (a clip short enough to fit under it). Whichever
  client actually resolved a video is tracked on `VideoInfo::client_ua` and
  travels with every later request for it (media fetch, `--play`, download),
  so a download never signs its request with the wrong client's User-Agent.
  `--diag`'s "full-length fetch" check probes near the *end* of a stream
  specifically to catch this class of break -- a near-start probe alone always
  passes, cap or no cap, because the cap only bites once you reach it.
- **This is a moving target.** If YouTube closes the `VISIONOS` gap the way it
  did `ANDROID_VR`'s, the fix is the same kind of fix: find whichever client
  in yt-dlp's current `INNERTUBE_CLIENTS` table has no `GVS_PO_TOKEN_POLICY`
  override and no `REQUIRE_JS_PLAYER`, and move it to the front of
  `PLAYER_CHAIN` in `yt_innertube.h`.
- **No SABR/UMP support.** If every client ends up requiring a PO Token, this
  approach stops working entirely and the gate/error says so rather than
  silently dropping to 360p.
